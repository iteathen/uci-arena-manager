#include "arena/adapters/sqlite_store.hpp"
#include "arena/application/control_plane.hpp"
#include "arena/domain/uuid.hpp"

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <memory>
#include <string>

namespace {

namespace fs = std::filesystem;
using nlohmann::json;

json command(
    const std::string& action,
    const json& target,
    const json& parameters = json::object()) {
    return {
        {"schema_version", 1},
        {"request_id", arena::domain::uuid_v7()},
        {"idempotency_key", arena::domain::uuid_v7()},
        {"action", action},
        {"target", target},
        {"dry_run", false},
        {"parameters", parameters},
    };
}

json register_and_status(
    arena::application::ControlPlane& control_plane,
    const fs::path& root,
    const std::string& profile,
    const std::string& cli_name) {
    const fs::path cli = root / cli_name;
    const fs::path workspace = root / (profile + "-workspace");
    fs::create_directories(workspace);
    std::ofstream(cli) << "mock";
    const json created = control_plane.submit(
        command(
            "service.register",
            {{"kind", "managed_service"}},
            {
                {"profile_id", profile},
                {"node_executable", ARENA_MOCK_SERVICE_PATH},
                {"cli_entrypoint", cli.string()},
                {"workspace", workspace.string()},
            }),
        "test");
    const std::string id =
        created.at("resource").at("id").get<std::string>();
    return control_plane.submit(
        command(
            "service.status",
            {{"kind", "managed_service"}, {"id", id}}),
        "test");
}

}  // namespace

TEST_CASE("Book Forge and Evidence outputs cross only validated public contracts") {
    const fs::path root =
        fs::temp_directory_path() /
        ("arena-service-output-" + arena::domain::uuid_v7());
    fs::create_directories(root);
    {
        auto store = std::make_shared<arena::adapters::SqliteStore>(
            root / "arena.db");
        arena::application::ControlPlane control_plane(store);
        control_plane.initialize();

        const json book = register_and_status(
            control_plane, root, "book_forge", "book-cli.mjs");
        const json& book_data = book.at("resource").at("data");
        REQUIRE(book_data.at("integration_state") == "available");
        REQUIRE(
            book_data.at("output_integration").at("authority") ==
            "book_forge");
        REQUIRE(
            book_data.at("output_integration")
                    .at("snapshots")
                    .at(0)
                    .at("manifest_sha256") ==
            std::string(64, 'a'));

        const json evidence = register_and_status(
            control_plane,
            root,
            "uci_arena_evidence",
            "evidence-cli.mjs");
        const json& evidence_data = evidence.at("resource").at("data");
        REQUIRE(evidence_data.at("integration_state") == "available");
        const json& dataset =
            evidence_data.at("output_integration").at("datasets").at(0);
        REQUIRE(dataset.at("id") == "dataset-1");
        REQUIRE(dataset.at("observation_count") == 12);
        REQUIRE(dataset.at("authority").at("password") == "[REDACTED]");
        REQUIRE_FALSE(dataset.contains("observations"));
    }
    fs::remove_all(root);
}
