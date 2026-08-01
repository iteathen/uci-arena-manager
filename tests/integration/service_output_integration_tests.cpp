#include "arena/adapters/sqlite_store.hpp"
#include "arena/application/control_plane.hpp"
#include "arena/application/service_catalog.hpp"
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

json register_service(
    arena::application::ControlPlane& control_plane,
    const fs::path& root,
    const std::string& profile,
    const std::string& cli_name) {
    const fs::path cli = root / cli_name;
    const fs::path workspace = root / (profile + "-workspace");
    fs::create_directories(workspace);
    std::ofstream(cli) << "mock";
    return control_plane.submit(
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
}

json register_and_status(
    arena::application::ControlPlane& control_plane,
    const fs::path& root,
    const std::string& profile,
    const std::string& cli_name) {
    const json created =
        register_service(
            control_plane, root, profile, cli_name);
    const std::string id =
        created.at("resource").at("id").get<std::string>();
    return control_plane.submit(
        command(
            "service.status",
            {{"kind", "managed_service"}, {"id", id}}),
        "test");
}

void seed_managed_service(
    const std::shared_ptr<arena::adapters::SqliteStore>& store,
    const std::string& id,
    const json& data,
    const json& labels = json::object(),
    const std::string& kind = "managed_service") {
    const std::string timestamp = "2026-01-01T00:00:00.000Z";
    const std::string command_id = arena::domain::uuid_v7();
    store->create_resource(
        {
            id,
            kind,
            "local",
            1,
            timestamp,
            timestamp,
            labels.dump(),
            data.dump(),
        },
        {
            command_id,
            arena::domain::uuid_v7(),
            arena::domain::uuid_v7(),
            "test.seed",
            "completed",
            "{}",
            timestamp,
        },
        {
            0,
            arena::domain::uuid_v7(),
            timestamp,
            "state",
            kind + ".created",
            kind,
            id,
            1,
            command_id,
            "{}",
        });
}

json register_installed_evidence_and_status(
    arena::application::ControlPlane& control_plane,
    const std::shared_ptr<arena::adapters::SqliteStore>& store,
    const fs::path& root) {
    const fs::path cli = root / "evidence-cli.mjs";
    const fs::path workspace = root / "uci_arena_evidence-workspace";
    fs::create_directories(workspace);
    std::ofstream(cli) << "mock";
    const json manager_api{
        {"schema", "uci_arena_manager_service_api_v1"},
        {"version", "1.0"},
        {"transport", "cli_json"},
        {"entrypoint", "cli"},
        {"arguments", {"api"}},
        {"operations",
         {"describe",
          "status",
          "outputs_list",
          "outputs_inspect",
          "config_validate",
          "orchestration_status",
          "target_register",
          "campaign_enqueue",
          "job_control"}},
        {"output_schema", "uci_arena_service_output_v1"},
    };
    const json data = arena::application::ServiceCatalog::normalize_registration({
        {"profile_id", "uci_arena_evidence"},
        {"node_executable", ARENA_MOCK_SERVICE_PATH},
        {"cli_entrypoint", cli.string()},
        {"workspace", workspace.string()},
        {"installed_component_id", "uci_arena_evidence.service"},
        {"installed_component_version", "0.3.0"},
        {"manager_integration_api", manager_api},
        {"manager_integration_api_source", "signed_component_manifest"},
    });
    const std::string id = arena::domain::uuid_v7();
    seed_managed_service(store, id, data, {{"managed_by", "arena-setup"}});
    return control_plane.submit(
        command(
            "service.status",
            {{"kind", "managed_service"}, {"id", id}}),
        "test");
}

}  // namespace

TEST_CASE("managed service profiles are cardinal and removal respects ownership") {
    const fs::path root =
        fs::temp_directory_path() /
        ("arena-service-cardinality-" + arena::domain::uuid_v7());
    fs::create_directories(root);
    {
        auto store = std::make_shared<arena::adapters::SqliteStore>(
            root / "arena.db");
        arena::application::ControlPlane control_plane(store);
        control_plane.initialize();

        const json manual = register_service(
            control_plane, root, "book_forge", "book-cli.mjs");
        REQUIRE_THROWS_AS(
            register_service(
                control_plane,
                root,
                "book_forge",
                "book-cli-duplicate.mjs"),
            arena::application::CommandFailure);

        const std::string manual_id =
            manual.at("resource").at("id").get<std::string>();
        static_cast<void>(control_plane.submit(
            command(
                "resource.remove",
                {{"kind", "managed_service"}, {"id", manual_id}}),
            "test"));
        REQUIRE(control_plane.list("managed_service").empty());

        const std::string active_id = arena::domain::uuid_v7();
        seed_managed_service(
            store,
            active_id,
            {
                {"profile_id", "book_forge"},
                {"observed_state", "running"},
            });
        REQUIRE_THROWS_AS(
            control_plane.submit(
                command(
                    "resource.remove",
                    {{"kind", "managed_service"}, {"id", active_id}}),
                "test"),
            arena::application::CommandFailure);

        const std::string receipt_owned_id = arena::domain::uuid_v7();
        seed_managed_service(
            store,
            receipt_owned_id,
            {
                {"profile_id", "uci_arena_evidence"},
                {"observed_state", "stopped"},
                {"installed_component_id", "uci_arena_evidence.service"},
            },
            {{"managed_by", "arena-setup"}});
        REQUIRE_THROWS_AS(
            control_plane.submit(
                command(
                    "resource.remove",
                    {{"kind", "managed_service"},
                     {"id", receipt_owned_id}}),
                "test"),
            arena::application::CommandFailure);
    }
    fs::remove_all(root);
}

TEST_CASE("service start never reports running before provider status") {
    const fs::path root =
        fs::temp_directory_path() /
        ("arena-service-start-" + arena::domain::uuid_v7());
    fs::create_directories(root);
    {
        auto store =
            std::make_shared<arena::adapters::SqliteStore>(
                root / "arena.db");
        arena::application::ControlPlane control_plane(store);
        control_plane.initialize();
        const json created = register_service(
            control_plane,
            root,
            "book_forge",
            "book-cli.mjs");
        const std::string id =
            created.at("resource").at("id");

        REQUIRE_THROWS_AS(
            control_plane.submit(
                command(
                    "service.start",
                    {{"kind", "managed_service"},
                     {"id", id}}),
                "test"),
            arena::application::CommandFailure);
        const json resources =
            control_plane.list("managed_service");
        REQUIRE(resources.size() == 1);
        const json& launch_state =
            resources.at(0).at("data");
        REQUIRE(launch_state.at("desired_state") == "stopped");
        REQUIRE(
            launch_state.at("observed_state") != "running");
        REQUIRE(
            launch_state.at("health").at("state") ==
            "unknown");

        const json status = control_plane.submit(
            command(
                "service.status",
                {{"kind", "managed_service"}, {"id", id}}),
            "test");
        REQUIRE(
            status.at("resource")
                    .at("data")
                    .at("observed_state") == "running");
    }
    fs::remove_all(root);
}

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
        REQUIRE(book_data.at("observed_state") == "running");
        REQUIRE(
            book_data.at("output_integration").at("authority") ==
            "book_forge");
        REQUIRE(
            book_data.at("output_integration")
                    .at("snapshots")
                    .at(0)
                    .at("manifest_sha256") ==
            std::string(64, 'a'));

        const json evidence = register_installed_evidence_and_status(
            control_plane, store, root);
        const json& evidence_data = evidence.at("resource").at("data");
        REQUIRE(evidence_data.at("integration_state") == "available");
        REQUIRE(evidence_data.at("observed_state") == "running");
        const json& dataset =
            evidence_data.at("output_integration").at("datasets").at(0);
        REQUIRE(
            dataset.at("id") ==
            "123e4567-e89b-42d3-a456-426614174000");
        REQUIRE(dataset.at("observation_count") == 12);
        REQUIRE(dataset.at("authority").at("password") == "[REDACTED]");
        REQUIRE_FALSE(dataset.contains("observations"));
        const json& timing_profile =
            evidence_data.at("output_integration")
                .at("selected_timing_profile");
        REQUIRE(
            evidence_data.at("output_integration")
                .at("timing_profile_count") == 1);
        REQUIRE(timing_profile.at("source") == "service_live_cache");
        REQUIRE(
            timing_profile.at("target_identity").at("model_id") ==
            "model-1");
        REQUIRE(
            timing_profile.at("service_publication").at("password") ==
            "[REDACTED]");
        REQUIRE(
            timing_profile.at("permissions")
                .at("provider_safety_qualified") == true);
        REQUIRE(
            evidence_data.at("manager_api_projection").at("schema") ==
            "uci_arena_manager_service_api_v1");
        REQUIRE(
            evidence_data.at("manager_api_projection")
                .at("operations").size() == 9);
        REQUIRE(
            evidence_data.at("orchestration_projection")
                .at("targets").size() == 1);
        REQUIRE(
            evidence_data.at("orchestration_projection")
                .at("jobs").at(0).at("state") == "queued");

        const std::string evidence_id =
            evidence.at("resource").at("id");
        const fs::path engine_root = root / "engine-component";
        const fs::path engine_executable =
#ifdef _WIN32
            engine_root / "bin" / "uci_arena_engine.exe";
#else
            engine_root / "bin" / "uci_arena_engine";
#endif
        fs::create_directories(engine_executable.parent_path());
        fs::create_directories(engine_root / "models" / "default");
        fs::copy_file(
            ARENA_MOCK_UCI_PATH,
            engine_executable,
            fs::copy_options::overwrite_existing);
        const std::string engine_id = arena::domain::uuid_v7();
        seed_managed_service(
            store,
            engine_id,
            {
                {"display_name", "UCI Arena Engine"},
                {"executable", engine_executable.string()},
                {"working_directory", engine_root.string()},
                {"arguments",
                 {"--require-model-root",
                  (engine_root / "models" / "default").string()}},
                {"option_overrides", json::object()},
                {"component_id", "uci_arena.engine"},
                {"onboarding_source", "install_receipt"},
                {"recommended", true},
                {"default_model",
                 {{"model_id", "example_model_v1"},
                  {"display_name", "Example Model"},
                  {"input_adapter_id", "dense_planes_v1"},
                  {"root", "models/default"}}},
            },
            json::object(),
            "engine");
        REQUIRE_THROWS_AS(
            control_plane.submit(
                command(
                    "service.evidence_target_register",
                    {{"kind", "managed_service"}, {"id", evidence_id}},
                    {{"engine_id", engine_id},
                     {"engine", "C:/browser-selected.exe"}}),
                "test"),
            arena::application::CommandFailure);
        const json target = control_plane.submit(
            command(
                "service.evidence_target_register",
                {{"kind", "managed_service"}, {"id", evidence_id}},
                {{"engine_id", engine_id}}),
            "test");
        REQUIRE(
            target.at("resource").at("data")
                .at("last_control_result").at("created") == true);
        REQUIRE(
            target.at("resource").at("data")
                .at("last_control_result").at("target").at("model_id") ==
            "example_model_v1");
        const json campaign = control_plane.submit(
            command(
                "service.evidence_campaign_enqueue",
                {{"kind", "managed_service"}, {"id", evidence_id}}),
            "test");
        REQUIRE(
            campaign.at("resource").at("data")
                .at("last_control_result").at("campaign") ==
            "time_control_curve_v1");
        const json cancelled = control_plane.submit(
            command(
                "service.evidence_job_cancel",
                {{"kind", "managed_service"}, {"id", evidence_id}},
                {{"job_id", "123e4567-e89b-42d3-a456-426614174002"}}),
            "test");
        REQUIRE(
            cancelled.at("resource").at("data")
                .at("last_control_result").at("job").at("state") ==
            "cancelled");
    }
    fs::remove_all(root);
}

TEST_CASE("Manager service API rejects an unknown major before output integration") {
    const fs::path root =
        fs::temp_directory_path() /
        ("arena-service-api-major-" + arena::domain::uuid_v7());
    fs::create_directories(root);
    {
        auto store = std::make_shared<arena::adapters::SqliteStore>(
            root / "arena.db");
        arena::application::ControlPlane control_plane(store);
        control_plane.initialize();

        const json result = register_and_status(
            control_plane, root, "book_forge", "book-bad-major.mjs");
        const json& data = result.at("resource").at("data");
        REQUIRE(data.at("integration_state") == "incompatible");
        REQUIRE(data.at("observed_state") == "degraded");
        REQUIRE_FALSE(data.contains("output_integration"));
        REQUIRE(
            data.at("health").at("reason") ==
            "service Manager API envelope is incompatible");
    }
    fs::remove_all(root);
}
