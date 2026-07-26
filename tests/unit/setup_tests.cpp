#include "arena/application/installer/setup.hpp"
#include "arena/domain/uuid.hpp"

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>

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
