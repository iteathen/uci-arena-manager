#include "arena/application/broker_policy.hpp"
#include "arena/application/service_catalog.hpp"
#include "arena/domain/uuid.hpp"

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>

namespace {

namespace fs = std::filesystem;
using nlohmann::json;

void write_json(const fs::path& path, const json& value) {
    fs::create_directories(path.parent_path());
    std::ofstream(path) << value.dump(2) << '\n';
}

json read_json(const fs::path& path) {
    std::ifstream input(path);
    json value;
    input >> value;
    return value;
}

void write_installed_component(
    const fs::path& data_root,
    const fs::path& install_root,
    const std::string& id,
    const std::string& entrypoint_name,
    const std::string& entrypoint_path,
    const std::vector<fs::path>& data_paths = {}) {
    const fs::path active = install_root / id / "1.0.0";
    const fs::path entrypoint = active / entrypoint_path;
    fs::create_directories(entrypoint.parent_path());
    std::ofstream(entrypoint, std::ios::binary) << "fixture";
    write_json(
        active / "arena-component.json",
        {
            {"schema", "arena_install_component_v1"},
            {"component_id", id},
            {"component_version", "1.0.0"},
            {"entrypoints", {{entrypoint_name, entrypoint_path}}},
            {"files",
             json::array(
                 {{{"path", entrypoint_path},
                   {"sha256",
                    "f16d05ec6b29248d2c61adb1e9263f78e4f7bace1b955014a2d17872cfe4064d"},
                   {"size_bytes", 7}}})},
        });
    json paths = json::array();
    for (const auto& path : data_paths) {
        fs::create_directories(path);
        paths.push_back(path.string());
    }
    write_json(
        data_root / "receipts" / (id + ".json"),
        {
            {"schema", "arena_install_receipt_v1"},
            {"component_id", id},
            {"component_version", "1.0.0"},
            {"state", "installed_managed"},
            {"active_root", active.string()},
            {"artifact_sha256", std::string(64, 'a')},
            {"data_paths", std::move(paths)},
        });
}

}  // namespace

TEST_CASE("UUIDv7 identifiers are valid and carry the version nibble") {
    const std::string id = arena::domain::uuid_v7();
    REQUIRE(arena::domain::is_uuid(id));
    REQUIRE(id.size() == 36);
    REQUIRE(id.at(14) == '7');
}

TEST_CASE("service catalog exposes both external services as optional and implemented") {
    const auto profiles = arena::application::ServiceCatalog::profiles();
    REQUIRE(profiles.size() == 2);
    REQUIRE(profiles.at(0).at("optional") == true);
    REQUIRE(profiles.at(1).at("optional") == true);
    REQUIRE(profiles.at(0).at("implementation_state") == "implemented_external");
    REQUIRE(profiles.at(1).at("implementation_state") == "implemented_external");
}

TEST_CASE("optional service discovery always exposes manual locator fallback") {
    const auto book =
        arena::application::ServiceCatalog::discover("book_forge");
    REQUIRE(book.at("optional") == true);
    REQUIRE(book.at("manual_configuration") ==
            nlohmann::json::array({"node_executable", "cli_entrypoint", "workspace"}));

    const auto evidence =
        arena::application::ServiceCatalog::discover("uci_arena_evidence");
    REQUIRE(evidence.at("manual_configuration") ==
            nlohmann::json::array({"node_executable", "cli_entrypoint", "workspace"}));
    REQUIRE_THROWS(
        arena::application::ServiceCatalog::normalize_registration(
            {{"profile_id", "uci_arena_evidence"}}));
}

TEST_CASE("service discovery consumes validated arena-setup locators without enabling them") {
    const fs::path root =
        fs::temp_directory_path() /
        ("arena-service-locator-" + arena::domain::uuid_v7());
    const fs::path data = root / "data";
    const fs::path provider = root / "Book Forge";
    fs::create_directories(data / "locators");
    fs::create_directories(provider / "app");
    const fs::path cli = provider / "app" / "book-forge.mjs";
    std::ofstream(cli) << "fixture";
    write_json(
        provider / "arena-component.json",
        {
            {"schema", "arena_install_component_v1"},
            {"component_id", "book_forge.service"},
            {"entrypoints", {{"cli", "app/book-forge.mjs"}}},
        });
    write_json(
        data / "locators" / "book_forge.json",
        {
            {"schema", "arena_saved_locator_v1"},
            {"schema_version", 1},
            {"kind", "book_forge"},
            {"mode", "in_place_reference"},
            {"path", provider.string()},
            {"validation", json::object()},
            {"saved_at", "2026-07-26T00:00:00Z"},
        });

    const auto discovered =
        arena::application::ServiceCatalog::discover("book_forge", data);
    REQUIRE(discovered.at("candidates").size() == 1);
    const auto& candidate = discovered.at("candidates").at(0);
    REQUIRE(candidate.at("source") == "saved_locator");
    REQUIRE(
        candidate.at("configuration").at("cli_entrypoint") ==
        fs::absolute(cli).lexically_normal().string());
    const auto missing =
        candidate.at("missing").get<std::vector<std::string>>();
    REQUIRE(
        std::find(missing.begin(), missing.end(), "workspace") !=
        missing.end());
    REQUIRE(candidate.at("complete") == false);
    fs::remove_all(root);
}

TEST_CASE("Evidence Service registration validates the built CLI process contract") {
    const auto directory =
        std::filesystem::temp_directory_path() /
        ("arena-evidence-profile-" + arena::domain::uuid_v7());
    std::filesystem::create_directories(directory);
    const auto node = directory / "node";
    const auto cli = directory / "cli.mjs";
    {
        std::ofstream(node) << "fixture";
        std::ofstream(cli) << "fixture";
    }
    const auto normalized = arena::application::ServiceCatalog::normalize_registration(
        {
            {"profile_id", "uci_arena_evidence"},
            {"node_executable", node.string()},
            {"cli_entrypoint", cli.string()},
            {"workspace", (directory / "workspace").string()},
        });
    REQUIRE(normalized.at("integration_state") == "configured");
    REQUIRE(normalized.at("observed_state") == "stopped");
    REQUIRE(normalized.at("transport") == "process");
    REQUIRE(normalized.at("health").at("state") == "unknown");
    REQUIRE(
        normalized.at("capabilities") ==
        arena::application::ServiceCatalog::profiles().at(1).at("capabilities"));
    std::filesystem::remove(node);
    std::filesystem::remove(cli);
    std::filesystem::remove(directory);
}

TEST_CASE("installed service receipts reconcile exact private runtime entrypoints") {
    const fs::path root =
        fs::temp_directory_path() /
        ("arena-installed-services-" + arena::domain::uuid_v7());
    const fs::path data = root / "data";
    const fs::path install = root / "install";
    write_installed_component(
        data,
        install,
        "node_runtime.private",
        "node",
        "bin/node");
    write_installed_component(
        data,
        install,
        "book_forge.service",
        "cli",
        "src/cli.mjs",
        {root / "workspaces" / "book-forge"});
    const fs::path service_receipt =
        data / "receipts" / "book_forge.service.json";
    const fs::path service_config =
        root / "workspaces" / "book-forge" / "manager-config.json";
    write_json(
        service_config,
        {
            {"schema", "flashfork_book_forge_config_v1"},
            {"stockfish", "C:/verified/stockfish.exe"},
        });
    json receipt = read_json(service_receipt);
    receipt["configuration_path"] = service_config.string();
    write_json(service_receipt, receipt);

    const auto discovered =
        arena::application::ServiceCatalog::installed_registrations(data);
    REQUIRE(discovered.at("errors").empty());
    REQUIRE(discovered.at("services").size() == 1);
    REQUIRE(
        discovered.at("services").at(0).at("profile_id") == "book_forge");
    REQUIRE(
        discovered.at("services").at(0).at("runtime_component_id") ==
        "node_runtime.private");
    REQUIRE(
        discovered.at("services").at(0).at("config_path") ==
        fs::absolute(service_config).string());

    std::ofstream(
        install / "book_forge.service" / "1.0.0" / "unlisted.mjs")
        << "untrusted";
    const auto unlisted =
        arena::application::ServiceCatalog::installed_registrations(data);
    REQUIRE(unlisted.at("services").empty());
    REQUIRE(unlisted.at("errors").size() == 1);
    fs::remove(
        install / "book_forge.service" / "1.0.0" / "unlisted.mjs");

    std::ofstream(
        install / "book_forge.service" / "1.0.0" / "src" / "cli.mjs",
        std::ios::app)
        << "tamper";
    const auto tampered =
        arena::application::ServiceCatalog::installed_registrations(data);
    REQUIRE(tampered.at("services").empty());
    REQUIRE(tampered.at("errors").size() == 1);
    fs::remove_all(root);
}

TEST_CASE("broker policy rejects unallowlisted services and actions") {
    const auto path =
        std::filesystem::temp_directory_path() /
        ("arena-broker-" + arena::domain::uuid_v7() + ".json");
    {
        std::ofstream output(path);
        output << R"({
          "schema_version": 1,
          "services": [{
            "id": "018f218e-7be1-7a56-9daa-3f003641c801",
            "platform_name": "ExampleService",
            "actions": ["status"]
          }]
        })";
    }
    const auto policy = arena::application::BrokerPolicy::load(path);
    REQUIRE(policy.size() == 1);
    REQUIRE(
        policy.authorize("018f218e-7be1-7a56-9daa-3f003641c801", "status").platform_name ==
        "ExampleService");
    REQUIRE_THROWS(
        policy.authorize("018f218e-7be1-7a56-9daa-3f003641c801", "restart"));
    REQUIRE_THROWS(
        policy.authorize("018f218e-7be1-7a56-9daa-3f003641c802", "status"));
    std::filesystem::remove(path);
}
