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

TEST_CASE("service catalog keeps planned services explicit and unavailable") {
    const auto profiles = arena::application::ServiceCatalog::profiles();
    REQUIRE(profiles.size() == 2);
    const auto planned = arena::application::ServiceCatalog::normalize_registration(
        {{"profile_id", "uci_arena_evidence"}});
    REQUIRE(planned.at("integration_state") == "planned");
    REQUIRE(planned.at("observed_state") == "unavailable");
    REQUIRE(planned.at("health").at("state") == "unavailable");
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
