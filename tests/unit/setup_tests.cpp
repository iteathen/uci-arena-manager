#include "arena/application/installer/setup.hpp"
#include "arena/domain/uuid.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <string>

namespace {

namespace fs = std::filesystem;
using arena::application::installer::SetupEngine;
using arena::application::installer::SetupPaths;
using nlohmann::json;

fs::path temporary_directory() {
    const fs::path directory =
        fs::temp_directory_path() / ("arena-setup-" + arena::domain::uuid_v7());
    fs::create_directories(directory);
    return directory;
}

json component(
    const std::string& id,
    const std::vector<std::string>& dependencies = {}) {
    return {
        {"id", id},
        {"version", "1.0.0"},
        {"dependencies", dependencies},
        {"installed_bytes", 1},
        {"staging_bytes", 1},
        {"backup_bytes", 0},
        {"workspace_reserve_bytes", 0},
        {"artifacts",
         {{"any",
           {{"source", id + ".tar.gz"},
            {"sha256", std::string(64, '0')},
            {"size_bytes", 1}}}}},
    };
}

SetupPaths paths_under(const fs::path& root) {
    fs::create_directories(root / "workspace");
    return {
        root / "install",
        root / "data",
        root / "cache",
        root / "workspace",
        root / "knowledge",
    };
}

}  // namespace

TEST_CASE("setup plan resolves dependencies once and includes Manager Core") {
    const fs::path root = temporary_directory();
    const json catalog{
        {"schema", "arena_install_catalog_v1"},
        {"schema_version", 1},
        {"suite_version", "1.0.0"},
        {"presets", {{"manager_only", json::array({"manager.core"})}}},
        {"components",
         json::array(
             {component("manager.core"),
              component("node_runtime.private"),
              component("book_forge.service", {"node_runtime.private"})})},
    };
    const auto plan = SetupEngine::plan(
        catalog,
        paths_under(root),
        "custom",
        {"book_forge.service", "node_runtime.private"},
        true,
        false);
    REQUIRE(plan.at("schema") == "arena_install_plan_v1");
    REQUIRE(plan.at("actions").size() == 3);
    REQUIRE(plan.at("actions").at(0).at("component_id") == "node_runtime.private");
    REQUIRE(plan.at("actions").at(1).at("component_id") == "book_forge.service");
    REQUIRE(plan.at("actions").at(2).at("component_id") == "manager.core");
    fs::remove_all(root);
}

TEST_CASE("download missing knowledge selects opening book and bounded Syzygy WDL") {
    const fs::path root = temporary_directory();
    const json catalog{
        {"schema", "arena_install_catalog_v1"},
        {"schema_version", 1},
        {"suite_version", "1.0.0"},
        {"presets", {{"manager_only", json::array({"manager.core"})}}},
        {"components",
         json::array(
             {component("manager.core"),
              component("opening_book.standard"),
              component("syzygy.wdl.3-4-5")})},
    };
    const auto plan = SetupEngine::plan(
        catalog,
        paths_under(root),
        "manager_only",
        {},
        false,
        true);
    REQUIRE(plan.at("download_missing_knowledge") == true);
    REQUIRE(plan.at("actions").size() == 3);
    REQUIRE(
        plan.at("actions").at(1).at("component_id") == "opening_book.standard");
    REQUIRE(
        plan.at("actions").at(2).at("component_id") == "syzygy.wdl.3-4-5");
    fs::remove_all(root);
}

TEST_CASE("missing books and tablebases can be selected independently") {
    const fs::path root = temporary_directory();
    const json catalog{
        {"schema", "arena_install_catalog_v1"},
        {"schema_version", 1},
        {"suite_version", "1.0.0"},
        {"presets", {{"manager_only", json::array({"manager.core"})}}},
        {"components",
         json::array(
             {component("manager.core"),
              component("opening_book.standard"),
              component("syzygy.wdl.3-4-5")})},
    };
    const auto books = SetupEngine::plan(
        catalog,
        paths_under(root),
        "manager_only",
        {},
        false,
        false,
        true,
        false);
    REQUIRE(books.at("download_missing_books") == true);
    REQUIRE(books.at("download_missing_tablebases") == false);
    REQUIRE(books.at("actions").size() == 2);
    REQUIRE(
        books.at("actions").at(1).at("component_id") ==
        "opening_book.standard");

    const auto tablebases = SetupEngine::plan(
        catalog,
        paths_under(root),
        "manager_only",
        {},
        false,
        false,
        false,
        true);
    REQUIRE(tablebases.at("download_missing_books") == false);
    REQUIRE(tablebases.at("download_missing_tablebases") == true);
    REQUIRE(tablebases.at("actions").size() == 2);
    REQUIRE(
        tablebases.at("actions").at(1).at("component_id") ==
        "syzygy.wdl.3-4-5");
    fs::remove_all(root);
}

TEST_CASE("inventory reports actionable missing and present knowledge assets") {
    const fs::path root = temporary_directory();
    const SetupPaths paths = paths_under(root);
    auto inventory = SetupEngine::inventory(paths);
    REQUIRE(inventory.at("knowledge").at("opening_book").at("state") == "missing");
    REQUIRE(inventory.at("knowledge").at("syzygy").at("state") == "missing");

    const fs::path book_root = paths.knowledge_root / "opening_books";
    const fs::path tablebase_root = paths.knowledge_root / "tablebases";
    fs::create_directories(book_root);
    fs::create_directories(tablebase_root);
    for (const std::string name :
         {"strong_rare_v1.bin",
          "strong_rare_v1.ffstats",
          "strong_rare_v1.policy",
          "snapshot.manifest.json"}) {
        std::ofstream(book_root / name) << "fixture";
    }
    std::ofstream(tablebase_root / "KQvK.rtbw") << "fixture";

    inventory = SetupEngine::inventory(paths);
    REQUIRE(
        inventory.at("knowledge").at("opening_book").at("state") ==
        "detected_unmanaged");
    REQUIRE(
        inventory.at("knowledge").at("syzygy").at("state") ==
        "detected_unmanaged");
    REQUIRE(inventory.at("knowledge").at("syzygy").at("wdl_files") == 1);

    const fs::path receipts = paths.data_root / "receipts";
    fs::create_directories(receipts);
    const auto activated = [](const fs::path& path) {
        return json{
            {"path", path.string()},
            {"size_bytes", fs::file_size(path)},
            {"sha256", std::string(64, '0')},
        };
    };
    std::ofstream(receipts / "opening_book.standard.json")
        << json{
               {"schema", "arena_install_receipt_v1"},
               {"component_id", "opening_book.standard"},
               {"state", "installed_managed"},
               {"activated_files",
                json::array({activated(book_root / "strong_rare_v1.bin")})},
           }
               .dump();
    std::ofstream(receipts / "syzygy.wdl.3-4-5.json")
        << json{
               {"schema", "arena_install_receipt_v1"},
               {"component_id", "syzygy.wdl.3-4-5"},
               {"state", "installed_managed"},
               {"activated_files",
                json::array({activated(tablebase_root / "KQvK.rtbw")})},
           }
               .dump();
    inventory = SetupEngine::inventory(paths);
    REQUIRE(
        inventory.at("knowledge").at("opening_book").at("state") ==
        "installed_managed");
    REQUIRE(
        inventory.at("knowledge").at("syzygy").at("state") ==
        "installed_managed");
    fs::remove_all(root);
}

TEST_CASE("file-set tablebases verify every file before atomic activation") {
    const fs::path root = temporary_directory();
    const SetupPaths paths = paths_under(root);
    constexpr const char* manifest_digest =
        "e23ab1788826a7bb5ef78e29553aadf1e429a9b2fccecf2087c6ca9beaa4f447";
    constexpr const char* manifest =
        R"({"schema":"arena_syzygy_file_set_manifest_v1","schema_version":1,"component_id":"syzygy.wdl.3-4-5","component_version":"1.0.0","total_size_bytes":11,"files":[{"path":"KQvK.rtbw","piece_count":3,"size_bytes":11,"sha256":"b905c2469583111c5d8dc8057dc426fcf74f17ec72adade00d8d7c80443b6d2f","sources":["KQvK.rtbw"]}]})";
    std::ofstream(root / "KQvK.rtbw", std::ios::binary) << "wdl-fixture";
    std::ofstream(root / "syzygy.file-set.json", std::ios::binary) << manifest;

    const json artifact{
        {"type", "file_set"},
        {"source", "syzygy.file-set.json"},
        {"sha256", manifest_digest},
        {"size_bytes", 312},
        {"file_count", 1},
        {"total_size_bytes", 11},
    };
    const json plan{
        {"schema", "arena_install_plan_v1"},
        {"blockers", json::array()},
        {"offline", true},
        {"catalog_directory", root.string()},
        {"actions",
         json::array(
             {{{"action", "install"},
               {"component_id", "syzygy.wdl.3-4-5"},
               {"version", "1.0.0"},
               {"enabled", true},
               {"artifact", artifact}}})},
    };
    const json applied =
        SetupEngine::apply(json::object(), plan, paths, true, false);
    REQUIRE(applied.at("state") == "committed");
    REQUIRE(
        fs::is_regular_file(
            paths.knowledge_root / "tablebases" / "KQvK.rtbw"));
    REQUIRE(
        fs::file_size(
            paths.knowledge_root / "tablebases" / "KQvK.rtbw") == 11);
    REQUIRE(SetupEngine::verify(paths).at("ok") == true);

    fs::remove_all(root);
}

TEST_CASE("file-set catalog totals fail closed before activation") {
    const fs::path root = temporary_directory();
    const SetupPaths paths = paths_under(root);
    constexpr const char* manifest =
        R"({"schema":"arena_syzygy_file_set_manifest_v1","schema_version":1,"component_id":"syzygy.wdl.3-4-5","component_version":"1.0.0","total_size_bytes":11,"files":[{"path":"KQvK.rtbw","piece_count":3,"size_bytes":11,"sha256":"b905c2469583111c5d8dc8057dc426fcf74f17ec72adade00d8d7c80443b6d2f","sources":["KQvK.rtbw"]}]})";
    std::ofstream(root / "KQvK.rtbw", std::ios::binary) << "wdl-fixture";
    std::ofstream(root / "syzygy.file-set.json", std::ios::binary) << manifest;
    const json plan{
        {"schema", "arena_install_plan_v1"},
        {"blockers", json::array()},
        {"offline", true},
        {"catalog_directory", root.string()},
        {"actions",
         json::array(
             {{{"action", "install"},
               {"component_id", "syzygy.wdl.3-4-5"},
               {"version", "1.0.0"},
               {"enabled", true},
               {"artifact",
                {{"type", "file_set"},
                 {"source", "syzygy.file-set.json"},
                 {"sha256",
                  "e23ab1788826a7bb5ef78e29553aadf1e429a9b2fccecf2087c6ca9beaa4f447"},
                 {"size_bytes", 312},
                 {"file_count", 1},
                 {"total_size_bytes", 12}}}}})},
    };
    REQUIRE_THROWS(
        SetupEngine::apply(json::object(), plan, paths, true, false));
    REQUIRE_FALSE(
        fs::exists(paths.knowledge_root / "tablebases" / "KQvK.rtbw"));
    fs::remove_all(root);
}

TEST_CASE("inventory returns bounded versioned discovery and browse contracts") {
    const fs::path root = temporary_directory();
    const SetupPaths paths = paths_under(root);
    const fs::path provider_root = root / "providers";
    const fs::path book_forge = provider_root / "book-forge";
    fs::create_directories(book_forge);
    std::ofstream(book_forge / "arena-component.json")
        << json{
               {"schema", "arena_install_component_v1"},
               {"component_id", "book_forge.service"},
               {"component_version", "1.0.0"},
           }
               .dump();

    const fs::path too_deep =
        provider_root / "nested" / "not-scanned";
    fs::create_directories(too_deep);
    std::ofstream(too_deep / "arena-component.json")
        << json{
               {"schema", "arena_install_component_v1"},
               {"component_id", "uci_arena_evidence.service"},
               {"component_version", "1.0.0"},
           }
               .dump();

    const json inventory =
        SetupEngine::inventory(paths, {provider_root});
    const json& book =
        inventory.at("discovery").at("book_forge");
    REQUIRE(book.at("schema") == "arena_setup_discovery_v1");
    REQUIRE(book.at("schema_version") == 1);
    REQUIRE(book.at("selection").at("source") == "provider_manifest");
    REQUIRE(
        book.at("browse").at("selection") == "file_or_directory");
    REQUIRE_FALSE(book.at("browse").at("hints").empty());
    REQUIRE(
        inventory.at("discovery")
            .at("opening_book")
            .at("browse")
            .at("managed_copy_allowed") == true);

    const auto& evidence_candidates =
        inventory.at("discovery")
            .at("evidence_service")
            .at("candidates");
    REQUIRE(std::none_of(
        evidence_candidates.begin(),
        evidence_candidates.end(),
        [&too_deep](const json& candidate) {
            return candidate.value("path", "") ==
                   fs::absolute(too_deep).lexically_normal().string();
        }));
    fs::remove_all(root);
}

TEST_CASE("selected asset roots discover books and Syzygy one level only") {
    const fs::path root = temporary_directory();
    const SetupPaths paths = paths_under(root);
    const fs::path asset_root = root / "mounted-assets";
    const fs::path first_books = asset_root / "a-books";
    const fs::path second_books = asset_root / "z-books";
    const fs::path syzygy = asset_root / "syzygy";
    const fs::path too_deep = asset_root / "nested" / "too-deep";
    fs::create_directories(first_books);
    fs::create_directories(second_books);
    fs::create_directories(syzygy);
    fs::create_directories(too_deep);
    std::ofstream(first_books / "first.bin") << "book-one";
    std::ofstream(second_books / "second.book") << "book-two";
    std::ofstream(syzygy / "KQvK.rtbw") << "tablebase";
    std::ofstream(too_deep / "hidden.rtbz") << "not-discovered";

    const json inventory = SetupEngine::inventory(paths, {asset_root});
    const auto& discovery = inventory.at("discovery");
    REQUIRE(
        discovery.at("opening_book").at("selection").at("source") ==
        "discovered");
    REQUIRE(
        discovery.at("opening_book").at("selection").at("path") ==
        fs::absolute(first_books).lexically_normal().string());
    REQUIRE(
        discovery.at("syzygy").at("selection").at("path") ==
        fs::absolute(syzygy).lexically_normal().string());
    const auto& syzygy_candidates =
        discovery.at("syzygy").at("candidates");
    REQUIRE(std::none_of(
        syzygy_candidates.begin(),
        syzygy_candidates.end(),
        [&too_deep](const json& candidate) {
            return candidate.value("path", "") ==
                   fs::absolute(too_deep).lexically_normal().string();
        }));

    fs::remove_all(root);
}

TEST_CASE("discovery precedence is receipt then locator then provider manifest") {
    const fs::path root = temporary_directory();
    const SetupPaths paths = paths_under(root);
    const fs::path provider_root = root / "book-forge";
    fs::create_directories(provider_root);
    std::ofstream(provider_root / "arena-component.json")
        << json{
               {"schema", "arena_install_component_v1"},
               {"component_id", "book_forge.service"},
               {"component_version", "1.0.0"},
           }
               .dump();
    const fs::path cli = provider_root / "book-forge.mjs";
    std::ofstream(cli) << "fixture";

    auto inventory =
        SetupEngine::inventory(paths, {provider_root});
    REQUIRE(
        inventory.at("discovery")
            .at("book_forge")
            .at("selection")
            .at("source") == "provider_manifest");

    const json imported =
        SetupEngine::import_path(paths, "book_forge", cli);
    REQUIRE(imported.at("mode") == "in_place_reference");
    inventory = SetupEngine::inventory(paths, {provider_root});
    REQUIRE(
        inventory.at("discovery")
            .at("book_forge")
            .at("selection")
            .at("source") == "saved_locator");

    const fs::path receipts = paths.data_root / "receipts";
    fs::create_directories(receipts);
    std::ofstream(receipts / "book_forge.service.json")
        << json{
               {"schema", "arena_install_receipt_v1"},
               {"component_id", "book_forge.service"},
               {"component_version", "1.0.0"},
               {"state", "installed_managed"},
               {"active_root", provider_root.string()},
           }
               .dump();
    inventory = SetupEngine::inventory(paths, {provider_root});
    REQUIRE(
        inventory.at("discovery")
            .at("book_forge")
            .at("selection")
            .at("source") == "install_receipt");
    fs::remove_all(root);
}

TEST_CASE("local imports validate UCI and never return Lichess token contents") {
    const fs::path root = temporary_directory();
    const SetupPaths paths = paths_under(root);
    const json engine = SetupEngine::import_path(
        paths, "uci_engine", ARENA_MOCK_UCI_PATH);
    REQUIRE(engine.at("schema") == "arena_setup_import_result_v1");
    REQUIRE(engine.at("validation").at("engine_name") == "Arena Mock Engine");
    REQUIRE(engine.at("validation").at("fingerprint").get<std::string>().size() == 64);

    const fs::path token = root / "lichess-token.txt";
    const std::string token_contents = "TEST_TOKEN_CONTENT_MUST_NOT_ESCAPE";
    std::ofstream(token) << token_contents;
    const json token_result =
        SetupEngine::import_path(paths, "lichess_token_path", token);
    REQUIRE(
        token_result.at("validation").at("validation") ==
        "path_reference_only");
    REQUIRE(token_result.dump().find(token_contents) == std::string::npos);
    REQUIRE_THROWS(
        SetupEngine::import_path(paths, "restaurant", root));
    fs::remove_all(root);
}

TEST_CASE("timing profile imports require qualified exact runtime identity") {
    const fs::path root = temporary_directory();
    const SetupPaths paths = paths_under(root);
    const json identity{
        {"engine",
         {{"executable_sha256", "sha256:" + std::string(64, 'a')}}},
        {"model",
         {{"model_fingerprint", "42"}, {"model_id", "model-1"}}},
        {"search", {{"provider_effective", "cpu"}}},
    };
    json profile{
        {"schema", "uci_time_profile_v3"},
        {"qualified", true},
        {"gates", {{"passed", true}}},
        {"permissions",
         {{"provider_safety_qualified", true},
          {"forecast_shadow_qualified", false},
          {"adaptive_play_qualified", false}}},
        {"identity", identity},
        {"identity_sha256",
         "sha256:fe440d1087301bf982f8e930212ef84b4809864abd49992c21b960962a91d15d"},
    };
    const fs::path profile_path = root / "uci_time_profile_v3.json";
    std::ofstream(profile_path) << profile.dump(2);

    const json imported =
        SetupEngine::import_path(paths, "timing_profile", profile_path);
    REQUIRE(imported.at("mode") == "in_place_reference");
    REQUIRE(
        imported.at("validation").at("validation") ==
        "qualified_uci_time_profile_v3");
    REQUIRE(imported.at("validation").at("model_id") == "model-1");
    REQUIRE(imported.at("validation").at("provider") == "cpu");
    const json inventory = SetupEngine::inventory(paths);
    REQUIRE(
        inventory.at("discovery")
            .at("timing_profile")
            .at("selection")
            .at("source") == "saved_locator");

    profile["permissions"]["provider_safety_qualified"] = false;
    const fs::path unsafe_profile_path = root / "unsafe-profile.json";
    std::ofstream(unsafe_profile_path) << profile.dump(2);
    REQUIRE_THROWS(
        SetupEngine::import_path(
            paths, "timing_profile", unsafe_profile_path));

    profile["permissions"]["provider_safety_qualified"] = true;
    profile["identity_sha256"] = "sha256:" + std::string(64, '0');
    const fs::path drifted_profile_path = root / "drifted-profile.json";
    std::ofstream(drifted_profile_path) << profile.dump(2);
    REQUIRE_THROWS(
        SetupEngine::import_path(
            paths, "timing_profile", drifted_profile_path));
    fs::remove_all(root);
}

TEST_CASE("managed copy is restricted to small flat opening books") {
    const fs::path root = temporary_directory();
    const SetupPaths paths = paths_under(root);
    const fs::path source = root / "small-book";
    fs::create_directories(source);
    std::ofstream(source / "book.bin") << "polyglot";
    std::ofstream(source / "book.policy") << "policy";

    const json imported =
        SetupEngine::import_path(paths, "opening_book", source, true);
    REQUIRE(imported.at("mode") == "managed_copy");
    REQUIRE(imported.at("state") == "installed_managed");
    REQUIRE(fs::is_regular_file(
        fs::path(imported.at("path").get<std::string>()) / "book.bin"));
    REQUIRE(
        SetupEngine::verify(paths).at("ok") == true);

    const json inventory = SetupEngine::inventory(paths);
    REQUIRE(
        inventory.at("discovery")
            .at("opening_book")
            .at("selection")
            .at("source") == "install_receipt");
    REQUIRE_THROWS(
        SetupEngine::import_path(paths, "syzygy", source, true));

    const fs::path nested = root / "nested-book";
    fs::create_directories(nested / "child");
    std::ofstream(nested / "book.bin") << "polyglot";
    REQUIRE_THROWS(
        SetupEngine::import_path(paths, "opening_book", nested, true));

    const fs::path too_many = root / "too-many-book-files";
    fs::create_directories(too_many);
    for (int index = 0; index < 65; ++index) {
        std::ofstream(
            too_many / ("book-" + std::to_string(index) + ".bin"))
            << "fixture";
    }
    REQUIRE_THROWS(
        SetupEngine::import_path(paths, "opening_book", too_many, true));
    fs::remove_all(root);
}

TEST_CASE("import rejects symbolic-link traversal when supported") {
    const fs::path root = temporary_directory();
    const SetupPaths paths = paths_under(root);
    const fs::path target = root / "book.bin";
    const fs::path link = root / "book-link.bin";
    std::ofstream(target) << "polyglot";
    std::error_code error;
    fs::create_symlink(target, link, error);
    if (!error) {
        REQUIRE_THROWS(
            SetupEngine::import_path(paths, "opening_book", link));
    }
    fs::remove_all(root);
}

TEST_CASE("unsigned catalogs require an explicit development override") {
    const fs::path root = temporary_directory();
    const fs::path catalog_path = root / "catalog.json";
    std::ofstream(catalog_path)
        << R"({"schema":"arena_install_catalog_v1","schema_version":1,"suite_version":"1.0.0","presets":{},"components":[]})";
    REQUIRE_THROWS(SetupEngine::load_catalog(catalog_path));
    REQUIRE(
        SetupEngine::load_catalog(catalog_path, {}, true).at("suite_version") ==
        "1.0.0");
    fs::remove_all(root);
}

TEST_CASE("verified release media is retained for later repair and downloads") {
    const fs::path root = temporary_directory();
    const SetupPaths paths = paths_under(root);
    const fs::path media = root / "installer-media";
    fs::create_directories(media / "payloads");
    std::ofstream(media / "payloads" / "manager.tar.gz") << "payload";
    const json catalog{
        {"schema", "arena_install_catalog_v1"},
        {"schema_version", 1},
        {"suite_version", "1.0.0"},
        {"presets", {{"manager_only", json::array({"manager.core"})}}},
        {"components",
         json::array(
             {{{"id", "manager.core"},
               {"version", "1.0.0"},
               {"dependencies", json::array()},
               {"installed_bytes", 7},
               {"staging_bytes", 7},
               {"backup_bytes", 0},
               {"workspace_reserve_bytes", 0},
               {"artifacts",
                {{SetupEngine::platform_key(),
                  {{"source", "payloads/manager.tar.gz"},
                   {"sha256",
                    "239f59ed55e737c77147cf55ad0c1b030b6d7ee748a7426952f9b852d5a935e5"},
                   {"size_bytes", 7}}}}}}})},
    };
    std::ofstream(media / "catalog.json") << catalog.dump(2);
    const json loaded =
        SetupEngine::load_catalog(media / "catalog.json", {}, true);
    const json retained = SetupEngine::persist_release_media(
        loaded, paths, media / "catalog.json");
    REQUIRE(
        retained.at("schema") == "arena_retained_release_media_v1");
    REQUIRE(fs::is_regular_file(
        paths.data_root / "release" / "catalog.json"));
    REQUIRE(fs::is_regular_file(
        paths.data_root / "release" / "payloads" / "manager.tar.gz"));

    fs::remove_all(media);
    const json reloaded = SetupEngine::load_catalog(
        paths.data_root / "release" / "catalog.json", {}, true);
    REQUIRE(reloaded.at("suite_version") == "1.0.0");
    fs::remove_all(root);
}

TEST_CASE("suite uninstall removes every receipt while preserving workspaces") {
    const fs::path root = temporary_directory();
    const SetupPaths paths = paths_under(root);
    const fs::path receipts = paths.data_root / "receipts";
    fs::create_directories(receipts);
    const auto install = [&](const std::string& id, const json& dependencies) {
        const fs::path component_root =
            paths.install_root / "components" / id / "1.0.0";
        fs::create_directories(component_root);
        std::ofstream(component_root / "payload") << id;
        std::ofstream(receipts / (id + ".json"))
            << json{
                   {"schema", "arena_install_receipt_v1"},
                   {"schema_version", 1},
                   {"component_id", id},
                   {"component_version", "1.0.0"},
                   {"state", "installed_managed"},
                   {"active_root", component_root.string()},
                   {"artifact_sha256", std::string(64, 'a')},
                   {"dependencies", dependencies},
                   {"data_paths", json::array()},
                   {"activated_files", json::array()},
               }
                   .dump(2);
        return component_root;
    };
    const fs::path manager =
        install("manager.core", json::array());
    const fs::path node =
        install("node_runtime.private", json::array());
    const fs::path provider =
        install(
            "optional.provider",
            json::array({"node_runtime.private"}));
    const fs::path preserved =
        paths.workspace_root / "provider" / "state.json";
    fs::create_directories(preserved.parent_path());
    std::ofstream(preserved) << "{}";

    REQUIRE_THROWS(
        SetupEngine::uninstall(
            paths,
            {"node_runtime.private"},
            true,
            false));
    const json result =
        SetupEngine::uninstall(
            paths,
            {},
            true,
            false,
            true);
    REQUIRE(result.at("all") == true);
    REQUIRE(result.at("components").size() == 3);
    REQUIRE_FALSE(fs::exists(manager));
    REQUIRE_FALSE(fs::exists(node));
    REQUIRE_FALSE(fs::exists(provider));
    REQUIRE(fs::is_regular_file(preserved));
    REQUIRE(
        std::ranges::none_of(
            fs::directory_iterator(receipts),
            [](const fs::directory_entry& entry) {
                return entry.path().extension() == ".json";
            }));
    fs::remove_all(root);
}
