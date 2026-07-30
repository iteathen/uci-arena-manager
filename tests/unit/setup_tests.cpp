#include <algorithm>
#include <array>
#include <catch2/catch_test_macros.hpp>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>

#include "arena/application/installer/setup.hpp"
#include "arena/domain/uuid.hpp"

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

json component(const std::string& id, const std::vector<std::string>& dependencies = {}) {
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
           {{"source", id + ".tar.gz"}, {"sha256", std::string(64, '0')}, {"size_bytes", 1}}}}},
    };
}

SetupPaths paths_under(const fs::path& root) {
    fs::create_directories(root / "workspace");
    return {
        root / "install", root / "data", root / "cache", root / "workspace", root / "knowledge",
    };
}

fs::path write_lichess_single_profile(const fs::path& root,
                                      const std::string& filename =
                                          "uci_arena_single_game_profile_v1.json") {
    const fs::path evidence_root = root / "arena" / "results";
    fs::create_directories(evidence_root);
    std::ofstream(evidence_root / "diagnostic.json", std::ios::binary)
        << "diagnostic";
    std::ofstream(evidence_root / "complete-game.json", std::ios::binary)
        << "complete-game";
    const json diagnostic{
        {"passed", true},
        {"artifact", "arena/results/diagnostic.json"},
        {"artifact_sha256",
         "sha256:5a695eea5b00a31f8aef7dbb89c8f798fab371246ac1549afe84b16420707b99"},
    };
    const json complete{
        {"passed", true},
        {"artifact", "arena/results/complete-game.json"},
        {"artifact_sha256",
         "sha256:405758a289456712858d5ce6161c65529a244312e4ec2c92117cacd26cc60f87"},
    };
    const fs::path profile_path = root / filename;
    std::ofstream(profile_path)
        << json{
               {"schema", "uci_arena_single_game_profile_v1"},
               {"qualified", true},
               {"evidence",
                {{"cursor_correctness", complete},
                 {"provider_parity", diagnostic},
                 {"deadline", diagnostic},
                 {"topology", complete},
                 {"complete_game", complete}}},
           }
               .dump(2);
    return profile_path;
}

void write_book_forge_service_install(
    const SetupPaths& paths,
    const std::optional<std::pair<std::string_view, std::string_view>>& contract) {
    const fs::path active_root =
        paths.install_root / "components" / "book_forge.service" / "0.2.0";
    const fs::path cli = active_root / "services" / "book-forge" / "src" / "cli.mjs";
    const fs::path contract_path =
        active_root / "services" / "book-forge" / "contracts" / "service-contract.json";
    fs::create_directories(cli.parent_path());
    fs::create_directories(paths.data_root / "receipts");
    std::ofstream(cli, std::ios::binary) << "book-forge";
    json files = json::array(
        {{{"path", "services/book-forge/src/cli.mjs"},
          {"size_bytes", 10},
          {"sha256", "f7628bf86d7b79dab3847e8f5101b3f82224d7bdf694a9817cc0bfc710d553ad"}}});
    json manifest{
        {"schema", "arena_install_component_v1"},
        {"schema_version", 1},
        {"component_id", "book_forge.service"},
        {"component_version", "0.2.0"},
        {"entrypoints", {{"cli", "services/book-forge/src/cli.mjs"}}},
        {"capabilities", json::array({"status", "start"})},
        {"data_paths", json::array({"workspace"})},
        {"startup",
         {{"windows", "per_user_task_scheduler"}, {"linux", "per_user_systemd"}}},
    };
    if (contract.has_value()) {
        fs::create_directories(contract_path.parent_path());
        std::ofstream(contract_path, std::ios::binary) << contract->first;
        files.push_back(
            {{"path", "services/book-forge/contracts/service-contract.json"},
             {"size_bytes", contract->first.size()},
             {"sha256", std::string(contract->second)}});
        manifest["contract_manifest"] =
            "services/book-forge/contracts/service-contract.json";
    }
    manifest["files"] = std::move(files);
    std::ofstream(active_root / "arena-component.json", std::ios::binary)
        << manifest.dump(2);

    json receipt{
        {"schema", "arena_install_receipt_v1"},
        {"schema_version", 1},
        {"component_id", "book_forge.service"},
        {"component_version", "0.2.0"},
        {"state", "installed_managed"},
        {"active_root", active_root.string()},
        {"artifact_sha256", std::string(64, 'a')},
        {"dependencies", json::array()},
        {"data_paths", json::array({(paths.workspace_root / "book-forge").string()})},
        {"configuration_path", ""},
        {"activated_files", json::array()},
    };
    if (contract.has_value()) {
        receipt["service_contract"] = {
            {"schema", "uci_arena_standalone_service_contract_v1"},
            {"path", "services/book-forge/contracts/service-contract.json"},
            {"sha256", std::string(contract->second)},
            {"configuration_schema", "uci_arena_book_forge_config_v1"},
            {"status_schema", "uci_arena_book_forge_status_v2"},
        };
    }
    std::ofstream(paths.data_root / "receipts" / "book_forge.service.json",
                  std::ios::binary)
        << receipt.dump(2);
}

} // namespace

TEST_CASE("setup plan resolves dependencies once and includes Manager Core") {
    const fs::path root = temporary_directory();
    const json catalog{
        {"schema", "arena_install_catalog_v1"},
        {"schema_version", 1},
        {"suite_version", "1.0.0"},
        {"presets", {{"manager_only", json::array({"manager.core"})}}},
        {"components", json::array({component("manager.core"), component("node_runtime.private"),
                                    component("book_forge.service", {"node_runtime.private"})})},
    };
    const auto plan =
        SetupEngine::plan(catalog, paths_under(root), "custom",
                          {"book_forge.service", "node_runtime.private"}, true, false);
    REQUIRE(plan.at("schema") == "arena_install_plan_v1");
    REQUIRE(plan.at("actions").size() == 3);
    REQUIRE(plan.at("actions").at(0).at("component_id") == "node_runtime.private");
    REQUIRE(plan.at("actions").at(1).at("component_id") == "book_forge.service");
    REQUIRE(plan.at("actions").at(2).at("component_id") == "manager.core");
    fs::remove_all(root);
}

TEST_CASE("repair plan restages components that are already current") {
    const fs::path root = temporary_directory();
    const SetupPaths paths = paths_under(root);
    const fs::path active_root = paths.install_root / "components" / "manager.core" / "1.0.0";
    fs::create_directories(active_root);
    std::ofstream(active_root / "payload", std::ios::binary) << "payload";
    std::ofstream(active_root / "arena-component.json")
        << json{
               {"schema", "arena_install_component_v1"},
               {"schema_version", 1},
               {"component_id", "manager.core"},
               {"component_version", "1.0.0"},
               {"files",
                json::array(
                    {{{"path", "payload"},
                      {"size_bytes", 7},
                      {"sha256",
                       "239f59ed55e737c77147cf55ad0c1b030b6d7ee748a7426952f9b852d5a935e5"}}})},
           }
               .dump(2);
    fs::create_directories(paths.data_root / "receipts");
    std::ofstream(
        paths.data_root / "receipts" /
        "manager.core.json")
        << json{
               {"schema", "arena_install_receipt_v1"},
               {"schema_version", 1},
               {"component_id", "manager.core"},
               {"component_version", "1.0.0"},
               {"state", "installed_managed"},
               {"active_root", active_root.string()},
           }
               .dump(2);
    const json catalog{
        {"schema", "arena_install_catalog_v1"},
        {"schema_version", 1},
        {"suite_version", "1.0.0"},
        {"presets", {{"manager_only", json::array({"manager.core"})}}},
        {"components", json::array({component("manager.core")})},
    };

    const json keep = SetupEngine::plan(catalog, paths, "manager_only", {}, true, false);
    REQUIRE(keep.at("actions").at(0).at("action") == "keep");

    const json repair =
        SetupEngine::plan(catalog, paths, "manager_only", {}, true, false, false, false, true);
    REQUIRE(repair.at("repair") == true);
    REQUIRE(repair.at("actions").at(0).at("action") == "repair");
    REQUIRE(repair.at("actions").at(0).contains("artifact"));
    fs::remove_all(root);
}

TEST_CASE("standalone service contracts are identity-bound and receipt-verified") {
    constexpr std::string_view valid_contract =
        R"({"schema":"uci_arena_standalone_service_contract_v1","component_id":"book_forge.service","component_version":"0.2.0","cli_entrypoint":"services/book-forge/src/cli.mjs","configuration_schema":"uci_arena_book_forge_config_v1","status_schema":"uci_arena_book_forge_status_v2","capabilities":["status","start"],"data_paths":["workspace"],"startup":{"windows":"per_user_task_scheduler","linux":"per_user_systemd"},"health_smoke":{"command":"status","success":"exit_zero_and_status_schema"},"outputs":{},"persistence":{"upgrade":"preserve_and_migrate","repair":"preserve","uninstall_default":"preserve"}})";
    constexpr std::string_view invalid_entrypoint_contract =
        R"({"schema":"uci_arena_standalone_service_contract_v1","component_id":"book_forge.service","component_version":"0.2.0","cli_entrypoint":"services/book-forge/src/wrong.mjs","configuration_schema":"uci_arena_book_forge_config_v1","status_schema":"uci_arena_book_forge_status_v2","capabilities":["status","start"],"data_paths":["workspace"],"startup":{"windows":"per_user_task_scheduler","linux":"per_user_systemd"},"health_smoke":{"command":"status","success":"exit_zero_and_status_schema"},"outputs":{},"persistence":{"upgrade":"preserve_and_migrate","repair":"preserve","uninstall_default":"preserve"}})";

    const fs::path valid_root = temporary_directory();
    const SetupPaths valid_paths = paths_under(valid_root);
    write_book_forge_service_install(
        valid_paths,
        std::pair{
            valid_contract,
            std::string_view{
                "eb5d969de295d01cbbb365c82d645798cc1e2c482e1f03ff664c0547bd49314a"}});
    REQUIRE(SetupEngine::verify(valid_paths).at("ok") == true);
    fs::remove_all(valid_root);

    const fs::path mismatched_root = temporary_directory();
    const SetupPaths mismatched_paths = paths_under(mismatched_root);
    write_book_forge_service_install(
        mismatched_paths,
        std::pair{
            invalid_entrypoint_contract,
            std::string_view{
                "45404169ac7dc063d4a41841a6e6d1ab430fbceee8f98a9f7f26317a80b7b2ae"}});
    REQUIRE(SetupEngine::verify(mismatched_paths).at("ok") == false);
    fs::remove_all(mismatched_root);

    const fs::path missing_root = temporary_directory();
    const SetupPaths missing_paths = paths_under(missing_root);
    write_book_forge_service_install(missing_paths, std::nullopt);
    REQUIRE(SetupEngine::verify(missing_paths).at("ok") == false);
    fs::remove_all(missing_root);
}

TEST_CASE("download missing knowledge selects opening book and bounded Syzygy WDL") {
    const fs::path root = temporary_directory();
    const json catalog{
        {"schema", "arena_install_catalog_v1"},
        {"schema_version", 1},
        {"suite_version", "1.0.0"},
        {"presets", {{"manager_only", json::array({"manager.core"})}}},
        {"components", json::array({component("manager.core"), component("opening_book.standard"),
                                    component("syzygy.wdl.3-4-5")})},
    };
    const auto plan =
        SetupEngine::plan(catalog, paths_under(root), "manager_only", {}, false, true);
    REQUIRE(plan.at("download_missing_knowledge") == true);
    REQUIRE(plan.at("actions").size() == 3);
    REQUIRE(plan.at("actions").at(1).at("component_id") == "opening_book.standard");
    REQUIRE(plan.at("actions").at(2).at("component_id") == "syzygy.wdl.3-4-5");
    fs::remove_all(root);
}

TEST_CASE("missing books and tablebases can be selected independently") {
    const fs::path root = temporary_directory();
    const json catalog{
        {"schema", "arena_install_catalog_v1"},
        {"schema_version", 1},
        {"suite_version", "1.0.0"},
        {"presets", {{"manager_only", json::array({"manager.core"})}}},
        {"components", json::array({component("manager.core"), component("opening_book.standard"),
                                    component("syzygy.wdl.3-4-5")})},
    };
    const auto books = SetupEngine::plan(catalog, paths_under(root), "manager_only", {}, false,
                                         false, true, false);
    REQUIRE(books.at("download_missing_books") == true);
    REQUIRE(books.at("download_missing_tablebases") == false);
    REQUIRE(books.at("actions").size() == 2);
    REQUIRE(books.at("actions").at(1).at("component_id") == "opening_book.standard");

    const auto tablebases = SetupEngine::plan(catalog, paths_under(root), "manager_only", {}, false,
                                              false, false, true);
    REQUIRE(tablebases.at("download_missing_books") == false);
    REQUIRE(tablebases.at("download_missing_tablebases") == true);
    REQUIRE(tablebases.at("actions").size() == 2);
    REQUIRE(tablebases.at("actions").at(1).at("component_id") == "syzygy.wdl.3-4-5");
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
    for (const std::string name : {"strong_rare_v1.bin", "strong_rare_v1.ffstats",
                                   "strong_rare_v1.policy", "snapshot.manifest.json"}) {
        std::ofstream(book_root / name) << "fixture";
    }
    std::ofstream(tablebase_root / "KQvK.rtbw") << "fixture";

    inventory = SetupEngine::inventory(paths);
    REQUIRE(inventory.at("knowledge").at("opening_book").at("state") == "detected_unmanaged");
    REQUIRE(inventory.at("knowledge").at("syzygy").at("state") == "detected_unmanaged");
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
    REQUIRE(inventory.at("knowledge").at("opening_book").at("state") == "installed_managed");
    REQUIRE(inventory.at("knowledge").at("syzygy").at("state") == "installed_managed");
    fs::remove_all(root);
}

TEST_CASE("file-set tablebases verify every file before atomic activation") {
    const fs::path root = temporary_directory();
    SetupPaths paths = paths_under(root);
    paths.bundle_owner = "{11111111-1111-1111-1111-111111111111}";
    constexpr const char* manifest_digest =
        "e23ab1788826a7bb5ef78e29553aadf1e429a9b2fccecf2087c6ca9beaa4f447";
    constexpr const char* manifest =
        R"({"schema":"arena_syzygy_file_set_manifest_v1","schema_version":1,"component_id":"syzygy.wdl.3-4-5","component_version":"1.0.0","total_size_bytes":11,"files":[{"path":"KQvK.rtbw","piece_count":3,"size_bytes":11,"sha256":"b905c2469583111c5d8dc8057dc426fcf74f17ec72adade00d8d7c80443b6d2f","sources":["KQvK.rtbw"]}]})";
    std::ofstream(root / "KQvK.rtbw", std::ios::binary) << "wdl-fixture";
    std::ofstream(root / "syzygy.file-set.json", std::ios::binary) << manifest;

    const json artifact{
        {"type", "file_set"},        {"source", "syzygy.file-set.json"},
        {"sha256", manifest_digest}, {"size_bytes", 312},
        {"file_count", 1},           {"total_size_bytes", 11},
    };
    const json plan{
        {"schema", "arena_install_plan_v1"},
        {"blockers", json::array()},
        {"offline", true},
        {"catalog_directory", root.string()},
        {"actions", json::array({{{"action", "install"},
                                  {"component_id", "syzygy.wdl.3-4-5"},
                                  {"version", "1.0.0"},
                                  {"enabled", true},
                                  {"artifact", artifact}}})},
    };
    const json applied = SetupEngine::apply(json::object(), plan, paths, true, false);
    REQUIRE(applied.at("state") == "committed");
    REQUIRE(applied.at("components").at(0).at("bundle_owner") == paths.bundle_owner);
    REQUIRE(fs::is_regular_file(paths.knowledge_root / "tablebases" / "KQvK.rtbw"));
    REQUIRE(fs::file_size(paths.knowledge_root / "tablebases" / "KQvK.rtbw") == 11);
    const fs::path locator_path = paths.data_root / "locators" / "syzygy.json";
    REQUIRE(fs::is_regular_file(locator_path));
    std::ifstream locator_stream(locator_path, std::ios::binary);
    const json locator = json::parse(locator_stream);
    REQUIRE(locator.at("kind") == "syzygy");
    REQUIRE(fs::path(locator.at("path").get<std::string>()) ==
            fs::absolute(paths.knowledge_root / "tablebases").lexically_normal());
    locator_stream.close();
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
        {"actions", json::array({{{"action", "install"},
                                  {"component_id", "syzygy.wdl.3-4-5"},
                                  {"version", "1.0.0"},
                                  {"enabled", true},
                                  {"artifact",
                                   {{"type", "file_set"},
                                    {"source", "syzygy.file-set.json"},
                                    {"sha256", "e23ab1788826a7bb5ef78e29553aadf1e429a9b2fcc"
                                               "ecf2087c6ca9beaa4f447"},
                                    {"size_bytes", 312},
                                    {"file_count", 1},
                                    {"total_size_bytes", 12}}}}})},
    };
    REQUIRE_THROWS(SetupEngine::apply(json::object(), plan, paths, true, false));
    REQUIRE_FALSE(fs::exists(paths.knowledge_root / "tablebases" / "KQvK.rtbw"));
    REQUIRE_FALSE(fs::exists(paths.data_root / "locators" / "syzygy.json"));
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

    const fs::path too_deep = provider_root / "nested" / "not-scanned";
    fs::create_directories(too_deep);
    std::ofstream(too_deep / "arena-component.json")
        << json{
               {"schema", "arena_install_component_v1"},
               {"component_id", "uci_arena_evidence.service"},
               {"component_version", "1.0.0"},
           }
               .dump();

    const json inventory = SetupEngine::inventory(paths, {provider_root});
    const json& book = inventory.at("discovery").at("book_forge");
    REQUIRE(book.at("schema") == "arena_setup_discovery_v1");
    REQUIRE(book.at("schema_version") == 1);
    REQUIRE(book.at("selection").at("source") == "provider_manifest");
    REQUIRE(book.at("browse").at("selection") == "file_or_directory");
    REQUIRE_FALSE(book.at("browse").at("hints").empty());
    REQUIRE(inventory.at("discovery").at("opening_book").at("browse").at("managed_copy_allowed") ==
            true);

    const auto& evidence_candidates =
        inventory.at("discovery").at("evidence_service").at("candidates");
    REQUIRE(std::none_of(evidence_candidates.begin(), evidence_candidates.end(),
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
    REQUIRE(discovery.at("opening_book").at("selection").at("source") == "discovered");
    REQUIRE(discovery.at("opening_book").at("selection").at("path") ==
            fs::absolute(first_books).lexically_normal().string());
    REQUIRE(discovery.at("syzygy").at("selection").at("path") ==
            fs::absolute(syzygy).lexically_normal().string());
    REQUIRE(inventory.at("knowledge").at("opening_book").at("state") ==
            "detected_unmanaged");
    REQUIRE(inventory.at("knowledge").at("opening_book").at("path") ==
            fs::absolute(first_books).lexically_normal().string());
    REQUIRE(inventory.at("knowledge").at("syzygy").at("state") ==
            "detected_unmanaged");
    REQUIRE(inventory.at("knowledge").at("syzygy").at("path") ==
            fs::absolute(syzygy).lexically_normal().string());
    REQUIRE(inventory.at("knowledge").at("syzygy").at("wdl_files") == 1);
    REQUIRE(inventory.at("knowledge").at("syzygy").at("dtz_files") == 0);
    REQUIRE(inventory.at("knowledge").at("syzygy").at("bytes") == 9);
    const auto& syzygy_candidates = discovery.at("syzygy").at("candidates");
    REQUIRE(std::none_of(syzygy_candidates.begin(), syzygy_candidates.end(),
                         [&too_deep](const json& candidate) {
                             return candidate.value("path", "") ==
                                    fs::absolute(too_deep).lexically_normal().string();
                         }));

    fs::remove_all(root);
}

TEST_CASE("saved in-place Syzygy locator is projected into knowledge inventory") {
    const fs::path root = temporary_directory();
    const SetupPaths paths = paths_under(root);
    const fs::path syzygy = root / "mounted-syzygy";
    fs::create_directories(syzygy);
    std::ofstream(syzygy / "KQvK.RTBW") << "wdl";
    std::ofstream(syzygy / "KQvK.rtbz") << "dtz-data";

    const json imported = SetupEngine::import_path(paths, "syzygy", syzygy);
    REQUIRE(imported.at("mode") == "in_place_reference");
    const json inventory = SetupEngine::inventory(paths);
    const json& knowledge = inventory.at("knowledge").at("syzygy");
    REQUIRE(knowledge.at("state") == "detected_unmanaged");
    REQUIRE(knowledge.at("source") == "saved_locator");
    REQUIRE(knowledge.at("path") == fs::absolute(syzygy).lexically_normal().string());
    REQUIRE(knowledge.at("wdl_files") == 1);
    REQUIRE(knowledge.at("dtz_files") == 1);
    REQUIRE(knowledge.at("bytes") == 11);

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

    auto inventory = SetupEngine::inventory(paths, {provider_root});
    REQUIRE(inventory.at("discovery").at("book_forge").at("selection").at("source") ==
            "provider_manifest");

    const json imported = SetupEngine::import_path(paths, "book_forge", cli);
    REQUIRE(imported.at("mode") == "in_place_reference");
    inventory = SetupEngine::inventory(paths, {provider_root});
    REQUIRE(inventory.at("discovery").at("book_forge").at("selection").at("source") ==
            "saved_locator");

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
    REQUIRE(inventory.at("discovery").at("book_forge").at("selection").at("source") ==
            "install_receipt");
    fs::remove_all(root);
}

TEST_CASE("local imports validate UCI and never return Lichess token contents") {
    const fs::path root = temporary_directory();
    const SetupPaths paths = paths_under(root);
    const json engine = SetupEngine::import_path(paths, "uci_engine", ARENA_MOCK_UCI_PATH);
    REQUIRE(engine.at("schema") == "arena_setup_import_result_v1");
    REQUIRE(engine.at("validation").at("engine_name") == "Arena Mock Engine");
    REQUIRE(engine.at("validation").at("fingerprint").get<std::string>().size() == 64);

    const fs::path token = root / "lichess-token.txt";
    const std::string token_contents = "TEST_TOKEN_CONTENT_MUST_NOT_ESCAPE";
    std::ofstream(token) << token_contents;
    const json token_result = SetupEngine::import_path(paths, "lichess_token_path", token);
    REQUIRE(token_result.at("validation").at("validation") == "path_reference_only");
    REQUIRE(token_result.dump().find(token_contents) == std::string::npos);
    REQUIRE_THROWS(SetupEngine::import_path(paths, "restaurant", root));
    fs::remove_all(root);
}

TEST_CASE("dashboard token reference save persists only path metadata") {
    const fs::path root = temporary_directory();
    const SetupPaths paths = paths_under(root);
    const fs::path token = root / "operator-token.txt";
    const std::string sentinel = "TOKEN_VALUE_MUST_NOT_ESCAPE";
    std::ofstream(token) << sentinel;

    const json result =
        SetupEngine::save_lichess_token_reference(paths.data_root, token);
    REQUIRE(result.at("schema") ==
            "arena_bot_token_reference_result_v1");
    REQUIRE(result.at("state") == "saved");
    REQUIRE(result.dump().find(sentinel) == std::string::npos);
    REQUIRE(result.dump().find(token.string()) == std::string::npos);

    const fs::path locator =
        paths.data_root / "locators" / "lichess_token_path.json";
    REQUIRE(fs::is_regular_file(locator));
    std::ifstream input(locator);
    const json metadata = json::parse(input);
    REQUIRE(metadata.at("validation").at("validation") ==
            "path_reference_only");
    REQUIRE(metadata.at("path") ==
            fs::absolute(token).lexically_normal().string());
    REQUIRE(metadata.dump().find(sentinel) == std::string::npos);
    input.close();

    const fs::path manager_owned_token =
        paths.data_root / "credentials" / "lichess.token";
    fs::create_directories(manager_owned_token.parent_path());
    std::ofstream(manager_owned_token) << "NON_SECRET_FIXTURE_VALUE";
    const auto require_manager_owned_rejection = [](const auto& operation) {
        try {
            operation();
            FAIL("Manager-owned credential path was accepted");
        } catch (const std::runtime_error& error) {
            REQUIRE(
                std::string(error.what()).find("outside Manager data") !=
                std::string::npos);
        }
    };
    require_manager_owned_rejection([&] {
        static_cast<void>(SetupEngine::save_lichess_token_reference(
            paths.data_root, manager_owned_token));
    });
    require_manager_owned_rejection([&] {
        static_cast<void>(SetupEngine::import_path(
            paths, "lichess_token_path", manager_owned_token));
    });
    fs::remove_all(root);
}

TEST_CASE("Lichess token path imports refresh installed Book Forge configuration") {
    const fs::path root = temporary_directory();
    const SetupPaths paths = paths_under(root);
    const fs::path receipts = paths.data_root / "receipts";
    const fs::path stockfish_root =
        paths.install_root / "components" / "stockfish.engine" / "1.0.0";
    const fs::path book_forge_root =
        paths.install_root / "components" / "book_forge.service" / "0.1.0";
    const fs::path stockfish = stockfish_root / "bin" / "stockfish.exe";
    const fs::path book_forge_cli = book_forge_root / "tools" / "book_forge" / "cli.mjs";
    const fs::path book_forge_config =
        paths.workspace_root / "book-forge" / "manager-config.json";
    fs::create_directories(stockfish.parent_path());
    fs::create_directories(book_forge_cli.parent_path());
    fs::create_directories(book_forge_config.parent_path());
    fs::create_directories(receipts);
    std::ofstream(stockfish, std::ios::binary) << "stockfish";
    std::ofstream(book_forge_cli, std::ios::binary) << "book-forge";
    std::ofstream(stockfish_root / "arena-component.json")
        << json{
               {"schema", "arena_install_component_v1"},
               {"schema_version", 1},
               {"component_id", "stockfish.engine"},
               {"component_version", "1.0.0"},
               {"entrypoints", {{"executable", "bin/stockfish.exe"}}},
               {"files",
                json::array(
                    {{{"path", "bin/stockfish.exe"},
                      {"size_bytes", 9},
                      {"sha256",
                       "92126dfcf0d4176dec9631a2a00a6d840ca672b62eccb67f637850e70aa9be8a"}}})},
           }
               .dump(2);
    std::ofstream(book_forge_root / "arena-component.json")
        << json{
               {"schema", "arena_install_component_v1"},
               {"schema_version", 1},
               {"component_id", "book_forge.service"},
               {"component_version", "0.1.0"},
               {"entrypoints", {{"cli", "tools/book_forge/cli.mjs"}}},
               {"files",
                json::array(
                    {{{"path", "tools/book_forge/cli.mjs"},
                      {"size_bytes", 10},
                      {"sha256",
                       "f7628bf86d7b79dab3847e8f5101b3f82224d7bdf694a9817cc0bfc710d553ad"}}})},
           }
               .dump(2);
    const auto write_receipt = [&](const std::string& id, const fs::path& active_root,
                                   const std::string& configuration_path = "",
                                   const std::string& component_version = "1.0.0") {
        std::ofstream(receipts / (id + ".json"))
            << json{
                   {"schema", "arena_install_receipt_v1"},
                   {"schema_version", 1},
                   {"component_id", id},
                   {"component_version", component_version},
                   {"state", "installed_managed"},
                   {"active_root", active_root.string()},
                   {"artifact_sha256", std::string(64, 'a')},
                   {"dependencies", json::array()},
                   {"data_paths", json::array()},
                   {"configuration_path", configuration_path},
                   {"activated_files", json::array()},
               }
                   .dump(2);
    };
    write_receipt("stockfish.engine", stockfish_root);
    write_receipt("book_forge.service", book_forge_root, book_forge_config.string(), "0.1.0");
    std::ofstream(book_forge_config)
        << json{
               {"schema", "uci_arena_book_forge_config_v1"},
               {"stockfish", stockfish.string()},
           }
               .dump(2);

    const fs::path token = root / "lichess-token.txt";
    const std::string token_contents = "TEST_TOKEN_CONTENT_MUST_NOT_ESCAPE";
    std::ofstream(token) << token_contents;
    const json imported = SetupEngine::import_path(paths, "lichess_token_path", token);
    REQUIRE(imported.at("refreshed_consumers") == json::array({"book_forge.service"}));
    std::ifstream generated_input(book_forge_config);
    json generated;
    generated_input >> generated;
    REQUIRE(generated.at("acquisition").at("tokenFile") ==
            fs::absolute(token).lexically_normal().string());
    REQUIRE(generated.at("stockfish") == fs::absolute(stockfish).lexically_normal().string());
    REQUIRE(generated.dump().find(token_contents) == std::string::npos);
    generated_input.close();
    fs::remove_all(root);
}

TEST_CASE("profile imports refresh a safely disabled installed Lichess adapter") {
    const fs::path root = temporary_directory();
    const SetupPaths paths = paths_under(root);
    const fs::path receipts = paths.data_root / "receipts";
    const fs::path engine_root =
        paths.install_root / "components" / "uci_arena.engine" / "1.0.0";
    const fs::path adapter_root =
        paths.install_root / "components" / "lichess.adapter" / "1.0.0";
    const fs::path engine = engine_root / "bin" / "uci_arena_engine";
    const fs::path checkpoint =
        engine_root / "models" / "default" / "checkpoint_manifest.json";
    const fs::path adapter = adapter_root / "app" / "adapter.mjs";
    const fs::path adapter_template =
        adapter_root / "deploy" / "lichess" /
        "config.uci_arena.template.yml";
    const fs::path configuration =
        paths.workspace_root / "lichess-adapter" / "manager-config.json";
    fs::create_directories(engine.parent_path());
    fs::create_directories(checkpoint.parent_path());
    fs::create_directories(adapter.parent_path());
    fs::create_directories(adapter_template.parent_path());
    fs::create_directories(configuration.parent_path());
    fs::create_directories(receipts);
    std::ofstream(engine, std::ios::binary) << "fixture";
    std::ofstream(checkpoint, std::ios::binary)
        << R"({"checkpoint_hash":"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa","model_id":"model-1"})";
    std::ofstream(adapter, std::ios::binary) << "fixture";
    std::ofstream(adapter_template, std::ios::binary)
        << R"({"schema":"uci_arena_lichess_adapter_v6","engine":{"uci_options":{}},"capacity":{"total_slots":1,"bot":{"target_active":0}},"scheduler":{"enabled":false},"timing":{}})";
    std::ofstream(engine_root / "arena-component.json")
        << json{
               {"schema", "arena_install_component_v1"},
               {"schema_version", 1},
               {"component_id", "uci_arena.engine"},
               {"component_version", "1.0.0"},
               {"entrypoints", {{"uci", "bin/uci_arena_engine"}}},
               {"files",
                json::array({
                    {
                        {"path", "bin/uci_arena_engine"},
                        {"size_bytes", 7},
                        {"sha256",
                         "f16d05ec6b29248d2c61adb1e9263f78e4f7bace1b955014a2d17872cfe4064d"},
                    },
                    {
                        {"path", "models/default/checkpoint_manifest.json"},
                        {"size_bytes", 107},
                        {"sha256",
                         "63d67d4aa35ea116824e4b267866f5a3e3a797f1a94ebb2e34eded84d479a511"},
                    },
                })},
           }
               .dump(2);
    std::ofstream(adapter_root / "arena-component.json")
        << json{
               {"schema", "arena_install_component_v1"},
               {"schema_version", 1},
               {"component_id", "lichess.adapter"},
               {"component_version", "1.0.0"},
               {"entrypoints", {{"adapter", "app/adapter.mjs"}}},
               {"files",
                json::array({
                    {
                        {"path", "app/adapter.mjs"},
                        {"size_bytes", 7},
                        {"sha256",
                         "f16d05ec6b29248d2c61adb1e9263f78e4f7bace1b955014a2d17872cfe4064d"},
                    },
                    {
                        {"path",
                         "deploy/lichess/config.uci_arena.template.yml"},
                        {"size_bytes", 166},
                        {"sha256",
                         "93c4b2eb1b64db5a021d92316ab162cdefd433ff2c19e19b62ca69294d29694b"},
                    },
                })},
           }
               .dump(2);
    const auto write_receipt = [&receipts](
                                   const std::string& id,
                                   const fs::path& active_root,
                                   const std::string& state,
                                   const fs::path& configuration_path =
                                       fs::path{}) {
        std::ofstream(receipts / (id + ".json"))
            << json{
                   {"schema", "arena_install_receipt_v1"},
                   {"schema_version", 1},
                   {"component_id", id},
                   {"component_version", "1.0.0"},
                   {"state", state},
                   {"active_root", active_root.string()},
                   {"artifact_sha256", std::string(64, 'a')},
                   {"dependencies", json::array()},
                   {"data_paths", json::array()},
                   {"configuration_path", configuration_path.string()},
                   {"activated_files", json::array()},
               }
                   .dump(2);
    };
    write_receipt(
        "uci_arena.engine", engine_root, "installed_managed");
    write_receipt(
        "lichess.adapter",
        adapter_root,
        "installed_disabled",
        configuration);
    std::ofstream(configuration)
        << json{
               {"schema", "uci_arena_lichess_adapter_v6"},
               {"engine", {{"uci_options", json::object()}}},
               {"capacity",
                {
                    {"total_slots", 1},
                    {"bot", {{"target_active", 0}}},
                }},
               {"scheduler", {{"enabled", false}}},
               {"timing", json::object()},
           }
               .dump(2);

    const fs::path transport = root / "transport_profile_v2.json";
    std::ofstream(transport)
        << json{
               {"schema", "uci_arena_lichess_transport_profile_v2"},
               {"qualified", true},
           }
               .dump(2);
    const json imported =
        SetupEngine::import_path(
            paths, "lichess_transport_profile", transport);
    REQUIRE(
        imported.at("refreshed_consumers") ==
        json::array({"lichess.adapter"}));
    std::ifstream updated_input(configuration);
    const json updated = json::parse(updated_input);
    REQUIRE(
        updated.at("timing").at("transport_profile_path") ==
        fs::absolute(transport).lexically_normal().string());
    updated_input.close();
    fs::remove_all(root);
}

TEST_CASE("timing profile imports require qualified exact runtime identity") {
    const fs::path root = temporary_directory();
    const SetupPaths paths = paths_under(root);
    const json identity{
        {"engine", {{"executable_sha256", "sha256:" + std::string(64, 'a')}}},
        {"model", {{"model_fingerprint", "42"}, {"model_id", "model-1"}}},
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
        {"identity_sha256", "sha256:"
                            "fe440d1087301bf982f8e930212ef84b4809864abd49992c21b960962a91d15d"},
    };
    const fs::path profile_path = root / "uci_time_profile_v3.json";
    std::ofstream(profile_path) << profile.dump(2);

    const json imported = SetupEngine::import_path(paths, "timing_profile", profile_path);
    REQUIRE(imported.at("mode") == "in_place_reference");
    REQUIRE(imported.at("validation").at("validation") == "qualified_uci_time_profile_v3");
    REQUIRE(imported.at("validation").at("model_id") == "model-1");
    REQUIRE(imported.at("validation").at("provider") == "cpu");
    const json inventory = SetupEngine::inventory(paths);
    REQUIRE(inventory.at("discovery").at("timing_profile").at("selection").at("source") ==
            "saved_locator");

    profile["permissions"]["provider_safety_qualified"] = false;
    const fs::path unsafe_profile_path = root / "unsafe-profile.json";
    std::ofstream(unsafe_profile_path) << profile.dump(2);
    REQUIRE_THROWS(SetupEngine::import_path(paths, "timing_profile", unsafe_profile_path));

    profile["permissions"]["provider_safety_qualified"] = true;
    profile["identity_sha256"] = "sha256:" + std::string(64, '0');
    const fs::path drifted_profile_path = root / "drifted-profile.json";
    std::ofstream(drifted_profile_path) << profile.dump(2);
    REQUIRE_THROWS(SetupEngine::import_path(paths, "timing_profile", drifted_profile_path));
    fs::remove_all(root);
}

TEST_CASE("Lichess safety profiles preserve qualification and manage the evidence closure") {
    const fs::path root = temporary_directory();
    const SetupPaths paths = paths_under(root);
    const std::array profiles{
        std::tuple{
            "lichess_single_game_profile",
            "uci_arena_single_game_profile_v1",
            "uci_arena_single_game_profile_v1.json",
        },
        std::tuple{
            "lichess_capacity_profile",
            "uci_arena_lichess_capacity_profile_v1",
            "uci_arena_lichess_capacity_profile_v1.json",
        },
        std::tuple{
            "lichess_transport_profile",
            "uci_arena_lichess_transport_profile_v2",
            "transport_profile_v2.json",
        },
    };
    for (const auto& [kind, schema, filename] : profiles) {
        const fs::path profile_path =
            std::string(kind) == "lichess_single_game_profile"
                ? write_lichess_single_profile(root, filename)
                : root / filename;
        if (std::string(kind) != "lichess_single_game_profile") {
            std::ofstream(profile_path)
                << json{{"schema", schema}, {"qualified", true}}.dump(2);
        }
        const json imported = SetupEngine::import_path(paths, kind, profile_path);
        REQUIRE(imported.at("mode") ==
                (std::string(kind) == "lichess_single_game_profile"
                     ? "managed_copy"
                     : "in_place_reference"));
        REQUIRE(imported.at("validation").at("validation") ==
                "qualified_" + std::string(schema));
        if (std::string(kind) == "lichess_single_game_profile") {
            REQUIRE(imported.at("path") != fs::absolute(profile_path).string());
            REQUIRE(fs::is_regular_file(imported.at("path").get<std::string>()));
            REQUIRE(imported.at("validation").at("evidence_files") == 2);
        }
    }

    const json inventory = SetupEngine::inventory(paths);
    for (const auto& [kind, schema, filename] : profiles) {
        (void)schema;
        (void)filename;
        const json& discovered = inventory.at("discovery").at(kind);
        REQUIRE(discovered.at("selection").at("source") == "saved_locator");
        REQUIRE_FALSE(discovered.at("browse").at("filters").empty());
        REQUIRE_FALSE(discovered.at("browse").at("hints").empty());
    }

    const fs::path unqualified = root / "unqualified.json";
    std::ofstream(unqualified)
        << json{
               {"schema", "uci_arena_lichess_capacity_profile_v1"},
               {"qualified", false},
           }
               .dump(2);
    REQUIRE_THROWS(
        SetupEngine::import_path(paths, "lichess_capacity_profile", unqualified));
    fs::remove_all(root);
}

TEST_CASE("Lichess installation is disabled without blocking on onboarding evidence") {
    const fs::path root = temporary_directory();
    const SetupPaths paths = paths_under(root);
    const json catalog{
        {"schema", "arena_install_catalog_v1"},
        {"schema_version", 1},
        {"suite_version", "1.0.0"},
        {"presets", {{"manager_only", json::array({"manager.core"})}}},
        {"components",
         json::array({component("manager.core"), component("lichess.adapter")})},
    };
    const json disabled =
        SetupEngine::plan(catalog, paths, "custom", {"lichess.adapter"}, true, false);
    REQUIRE(disabled.at("blockers").empty());
    const auto adapter = std::find_if(
        disabled.at("actions").begin(), disabled.at("actions").end(),
        [](const json& action) {
            return action.value("component_id", "") == "lichess.adapter";
        });
    REQUIRE(adapter != disabled.at("actions").end());
    REQUIRE(adapter->at("enabled") == false);

    const json identity{
        {"engine", {{"executable_sha256", "sha256:" + std::string(64, 'a')}}},
        {"model", {{"model_fingerprint", "42"}, {"model_id", "model-1"}}},
        {"search", {{"provider_effective", "cpu"}}},
    };
    const fs::path timing = root / "uci_time_profile_v3.json";
    std::ofstream(timing)
        << json{
               {"schema", "uci_time_profile_v3"},
               {"qualified", true},
               {"gates", {{"passed", true}}},
               {"permissions",
                {{"provider_safety_qualified", true},
                 {"forecast_shadow_qualified", false},
                 {"adaptive_play_qualified", false}}},
               {"identity", identity},
               {"identity_sha256",
                "sha256:"
                "fe440d1087301bf982f8e930212ef84b4809864abd49992c21b960962a91d15d"},
           }
               .dump(2);
    static_cast<void>(SetupEngine::import_path(paths, "timing_profile", timing));
    for (const auto& [kind, schema, filename] :
         std::array{
             std::tuple{
                 "lichess_single_game_profile",
                 "uci_arena_single_game_profile_v1",
                 "uci_arena_single_game_profile_v1.json",
             },
             std::tuple{
                 "lichess_capacity_profile",
                 "uci_arena_lichess_capacity_profile_v1",
                 "uci_arena_lichess_capacity_profile_v1.json",
             },
             std::tuple{
                 "lichess_transport_profile",
                 "uci_arena_lichess_transport_profile_v2",
                 "transport_profile_v2.json",
             },
         }) {
        const fs::path profile_path =
            std::string(kind) == "lichess_single_game_profile"
                ? write_lichess_single_profile(root, filename)
                : root / filename;
        if (std::string(kind) != "lichess_single_game_profile") {
            std::ofstream(profile_path)
                << json{{"schema", schema}, {"qualified", true}}.dump(2);
        }
        static_cast<void>(SetupEngine::import_path(paths, kind, profile_path));
    }
    const json ready =
        SetupEngine::plan(catalog, paths, "custom", {"lichess.adapter"}, true, false);
    REQUIRE(ready.at("blockers").empty());
    const auto still_disabled = std::find_if(
        ready.at("actions").begin(), ready.at("actions").end(),
        [](const json& action) {
            return action.value("component_id", "") == "lichess.adapter";
        });
    REQUIRE(still_disabled != ready.at("actions").end());
    REQUIRE(still_disabled->at("enabled") == false);
    fs::remove_all(root);
}

TEST_CASE("managed copy is restricted to small flat opening books") {
    const fs::path root = temporary_directory();
    const SetupPaths paths = paths_under(root);
    const fs::path source = root / "small-book";
    fs::create_directories(source);
    std::ofstream(source / "book.bin") << "polyglot";
    std::ofstream(source / "book.policy") << "policy";

    const json imported = SetupEngine::import_path(paths, "opening_book", source, true);
    REQUIRE(imported.at("mode") == "managed_copy");
    REQUIRE(imported.at("state") == "installed_managed");
    REQUIRE(fs::is_regular_file(fs::path(imported.at("path").get<std::string>()) / "book.bin"));
    REQUIRE(SetupEngine::verify(paths).at("ok") == true);

    const json inventory = SetupEngine::inventory(paths);
    REQUIRE(inventory.at("discovery").at("opening_book").at("selection").at("source") ==
            "install_receipt");
    REQUIRE_THROWS(SetupEngine::import_path(paths, "syzygy", source, true));

    const fs::path nested = root / "nested-book";
    fs::create_directories(nested / "child");
    std::ofstream(nested / "book.bin") << "polyglot";
    REQUIRE_THROWS(SetupEngine::import_path(paths, "opening_book", nested, true));

    const fs::path too_many = root / "too-many-book-files";
    fs::create_directories(too_many);
    for (int index = 0; index < 65; ++index) {
        std::ofstream(too_many / ("book-" + std::to_string(index) + ".bin")) << "fixture";
    }
    REQUIRE_THROWS(SetupEngine::import_path(paths, "opening_book", too_many, true));
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
        REQUIRE_THROWS(SetupEngine::import_path(paths, "opening_book", link));
    }
    fs::remove_all(root);
}

TEST_CASE("unsigned catalogs require an explicit development override") {
    const fs::path root = temporary_directory();
    const fs::path catalog_path = root / "catalog.json";
    std::ofstream(catalog_path)
        << R"({"schema":"arena_install_catalog_v1","schema_version":1,"suite_version":"1.0.0","presets":{},"components":[]})";
    REQUIRE_THROWS(SetupEngine::load_catalog(catalog_path));
    REQUIRE(SetupEngine::load_catalog(catalog_path, {}, true).at("suite_version") == "1.0.0");
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
        {"components", json::array({{{"id", "manager.core"},
                                     {"version", "1.0.0"},
                                     {"dependencies", json::array()},
                                     {"installed_bytes", 7},
                                     {"staging_bytes", 7},
                                     {"backup_bytes", 0},
                                     {"workspace_reserve_bytes", 0},
                                     {"artifacts",
                                      {{SetupEngine::platform_key(),
                                        {{"source", "payloads/manager.tar.gz"},
                                         {"sha256", "239f59ed55e737c77147cf55ad0c1b030b6d7e"
                                                    "e748a7426952f9b852d5a935e5"},
                                         {"size_bytes", 7}}}}}}})},
    };
    std::ofstream(media / "catalog.json") << catalog.dump(2);
    const json loaded = SetupEngine::load_catalog(media / "catalog.json", {}, true);
    const json retained = SetupEngine::persist_release_media(
        loaded, paths, media / "catalog.json", {}, true);
    REQUIRE(retained.at("schema") == "arena_retained_release_media_v1");
    REQUIRE(fs::is_regular_file(paths.data_root / "release" / "catalog.json"));
    REQUIRE(fs::is_regular_file(paths.data_root / "release" / "payloads" / "manager.tar.gz"));
    REQUIRE(
        fs::is_regular_file(
            paths.data_root / "release" /
            "development-trust.json"));
    REQUIRE_THROWS(
        SetupEngine::load_catalog(
            media / "catalog.json",
            {},
            false,
            {},
            paths.data_root / "release" /
                "development-trust.json"));

    fs::remove_all(media);
    const json reloaded =
        SetupEngine::load_catalog(
            paths.data_root / "release" / "catalog.json",
            {},
            false,
            {},
            paths.data_root / "release" /
                "development-trust.json");
    REQUIRE(reloaded.at("suite_version") == "1.0.0");
    REQUIRE(
        reloaded.at("_catalog_trust") ==
        "retained_unsigned_development");

    std::ofstream(
        paths.data_root / "release" / "catalog.json",
        std::ios::app)
        << ' ';
    REQUIRE_THROWS(
        SetupEngine::load_catalog(
            paths.data_root / "release" / "catalog.json",
            {},
            false,
            {},
            paths.data_root / "release" /
                "development-trust.json"));
    fs::remove_all(root);
}

TEST_CASE("suite uninstall removes every receipt while preserving workspaces") {
    const fs::path root = temporary_directory();
    const SetupPaths paths = paths_under(root);
    const fs::path receipts = paths.data_root / "receipts";
    fs::create_directories(receipts);
    const auto install = [&](const std::string& id, const json& dependencies,
                             const std::string& state = "installed_managed") {
        const fs::path component_root = paths.install_root / "components" / id / "1.0.0";
        fs::create_directories(component_root);
        std::ofstream(component_root / "payload") << id;
        std::ofstream(receipts / (id + ".json"))
            << json{
                   {"schema", "arena_install_receipt_v1"},
                   {"schema_version", 1},
                   {"component_id", id},
                   {"component_version", "1.0.0"},
                   {"state", state},
                   {"active_root", component_root.string()},
                   {"artifact_sha256", std::string(64, 'a')},
                   {"dependencies", dependencies},
                   {"data_paths", json::array()},
                   {"activated_files", json::array()},
               }
                   .dump(2);
        return component_root;
    };
    const fs::path manager = install("manager.core", json::array());
    const fs::path node = install("node_runtime.private", json::array());
    const fs::path provider =
        install("optional.provider", json::array({"node_runtime.private"}),
                "installed_disabled");
    const fs::path preserved = paths.workspace_root / "provider" / "state.json";
    fs::create_directories(preserved.parent_path());
    std::ofstream(preserved) << "{}";
    const std::array<fs::path, 8> retained_manager_state{
        paths.data_root / "arena.db",
        paths.data_root / "client.key",
        paths.data_root / "locators" / "opening_book.json",
        paths.data_root / "release" / "catalog.json",
        paths.data_root / "trust" / "tuf-versions.json",
        paths.data_root / "cache" / std::string(64, 'a'),
        paths.data_root / "transactions" / "completed.json",
        paths.knowledge_root / "opening_books" / "operator-book.bin",
    };
    for (const auto& path : retained_manager_state) {
        fs::create_directories(path.parent_path());
        std::ofstream(path) << "retained";
    }

    REQUIRE_THROWS(SetupEngine::uninstall(paths, {"node_runtime.private"}, true, false));
    const json result = SetupEngine::uninstall(paths, {}, true, false, true);
    REQUIRE(result.at("all") == true);
    REQUIRE(result.at("components").size() == 3);
    REQUIRE_FALSE(fs::exists(manager));
    REQUIRE_FALSE(fs::exists(node));
    REQUIRE_FALSE(fs::exists(provider));
    REQUIRE_FALSE(fs::exists(paths.install_root / "components" / "manager.core"));
    REQUIRE_FALSE(fs::exists(paths.install_root / "components" / "node_runtime.private"));
    REQUIRE_FALSE(fs::exists(paths.install_root / "components" / "optional.provider"));
    REQUIRE(fs::is_regular_file(preserved));
    for (const auto& path : retained_manager_state) {
        REQUIRE(fs::is_regular_file(path));
    }
    REQUIRE(std::ranges::none_of(
        fs::directory_iterator(receipts),
        [](const fs::directory_entry& entry) { return entry.path().extension() == ".json"; }));
    fs::remove_all(root);
}

TEST_CASE("older bundle uninstall cannot remove receipts adopted by a newer bundle") {
    const fs::path root = temporary_directory();
    SetupPaths paths = paths_under(root);
    const fs::path receipts = paths.data_root / "receipts";
    const fs::path component_root = paths.install_root / "components" / "manager.core" / "1.0.0";
    fs::create_directories(receipts);
    fs::create_directories(component_root);
    std::ofstream(component_root / "payload") << "manager";
    std::ofstream(receipts / "manager.core.json")
        << json{
               {"schema", "arena_install_receipt_v1"},
               {"schema_version", 1},
               {"component_id", "manager.core"},
               {"component_version", "1.0.0"},
               {"state", "installed_managed"},
               {"active_root", component_root.string()},
               {"artifact_sha256", std::string(64, 'a')},
               {"bundle_owner", "{22222222-2222-2222-2222-222222222222}"},
               {"dependencies", json::array()},
               {"data_paths", json::array()},
               {"activated_files", json::array()},
           }
               .dump(2);

    paths.bundle_owner = "{11111111-1111-1111-1111-111111111111}";
    const json old_uninstall = SetupEngine::uninstall(paths, {}, true, false, true);
    REQUIRE(old_uninstall.at("components").empty());
    REQUIRE(old_uninstall.at("skipped_foreign_receipts") == 1);
    REQUIRE(fs::is_regular_file(component_root / "payload"));
    REQUIRE(fs::is_regular_file(receipts / "manager.core.json"));

    paths.bundle_owner = "{22222222-2222-2222-2222-222222222222}";
    const json current_uninstall = SetupEngine::uninstall(paths, {}, true, false, true);
    REQUIRE(current_uninstall.at("components").size() == 1);
    REQUIRE_FALSE(fs::exists(component_root));
    REQUIRE_FALSE(fs::exists(receipts / "manager.core.json"));
    fs::remove_all(root);
}
