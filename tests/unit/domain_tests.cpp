#include "arena/application/broker_policy.hpp"
#include "arena/application/service_catalog.hpp"
#include "arena/domain/uuid.hpp"

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>

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
