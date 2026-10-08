#include "arena/adapters/sqlite_store.hpp"
#include "arena/application/control_plane.hpp"
#include "arena/application/engine_onboarding.hpp"
#include "arena/application/service_catalog.hpp"
#include "arena/domain/uuid.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_message.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <sodium.h>

#include <filesystem>
#include <array>
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
    const std::string& cli_name,
    const fs::path& node_executable = ARENA_MOCK_SERVICE_PATH) {
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
                {"node_executable", node_executable.string()},
                {"cli_entrypoint", cli.string()},
                {"workspace", workspace.string()},
            }),
        "test");
}

json register_and_status(
    arena::application::ControlPlane& control_plane,
    const fs::path& root,
    const std::string& profile,
    const std::string& cli_name,
    const fs::path& node_executable = ARENA_MOCK_SERVICE_PATH) {
    const json created =
        register_service(
            control_plane, root, profile, cli_name, node_executable);
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
    const fs::path& root,
    const std::string& cli_name = "evidence-cli.mjs") {
    const fs::path cli = root / cli_name;
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

TEST_CASE("Evidence forwards a freshly probed receipt-bound UCI launch without supplier inference") {
    const std::string supplier = GENERATE("uci_arena.vector", "example.alternate-engine");
    const bool capability = GENERATE(true, false);
    const fs::path root = fs::temp_directory_path() / ("arena-evidence-neutral-" + arena::domain::uuid_v7());
    const fs::path data_root = root / "data";
    const fs::path active = root / "engine";
    const fs::path executable = active / fs::path(ARENA_MOCK_UCI_PATH).filename();
    fs::create_directories(active);
    fs::create_directories(data_root / "receipts");
    fs::copy_file(ARENA_MOCK_UCI_PATH, executable);
    std::ofstream(active / "evidence-runtime.json") << R"({"schema":"uci_arena_evidence_runtime_v2"})";
    const auto inventory = [](const fs::path& file, const fs::path& component_root) {
        std::ifstream input(file, std::ios::binary);
        const std::string bytes{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
        std::array<unsigned char, crypto_hash_sha256_BYTES> hash{};
        crypto_hash_sha256(hash.data(), reinterpret_cast<const unsigned char*>(bytes.data()), bytes.size());
        std::array<char, crypto_hash_sha256_BYTES * 2 + 1> encoded{};
        sodium_bin2hex(encoded.data(), encoded.size(), hash.data(), hash.size());
        return json{{"path", fs::relative(file, component_root).generic_string()},
            {"sha256", encoded.data()}, {"size_bytes", fs::file_size(file)}};
    };
    const auto receipt = [&](const std::string& id, const fs::path& component_root, json manifest) {
        manifest["schema"] = "arena_install_component_v1";
        manifest["schema_version"] = 1;
        manifest["component_id"] = id;
        manifest["component_version"] = "0.3.0";
        std::ofstream(component_root / "arena-component.json") << manifest.dump();
        std::ofstream(data_root / "receipts" / (id + ".json")) << json{
            {"schema", "arena_install_receipt_v1"}, {"schema_version", 1},
            {"component_id", id}, {"component_version", "0.3.0"},
            {"state", "installed_managed"}, {"active_root", component_root.string()},
            {"artifact_sha256", std::string(64, 'a')}}.dump();
    };
    receipt(supplier, active, {
        {"entrypoints", {{"uci", executable.filename().generic_string()}, {"evidence_runtime_contract", "evidence-runtime.json"}}},
        {"discovery", json::array({{{"kind", "uci_engine"}, {"locator", {{"source", "entrypoint"}, {"entrypoint", "uci"}}}}})},
        {"capabilities", capability ? json::array({"evidence_runtime_contract_v2"}) : json::array()},
        {"files", {inventory(executable, active), inventory(active / "evidence-runtime.json", active)}}});
    {
        auto store = std::make_shared<arena::adapters::SqliteStore>(root / "arena.db");
        arena::application::ControlPlane control(store);
        control.initialize();
        const fs::path service_root = root / "service";
        fs::create_directories(service_root);
        const json evidence = register_installed_evidence_and_status(control, store, service_root, "evidence-neutral.mjs");
        receipt("uci_arena_evidence.service", service_root, {
            {"entrypoints", {{"cli", "evidence-neutral.mjs"}}},
            {"files", {inventory(service_root / "evidence-neutral.mjs", service_root)}}});
        control.begin_installed_reconciliation(data_root);
        // Reconciliation waits for the same complete registry verification used in production.
        control.reconcile_installed_services(data_root);
        const json found = control.engine_candidates();
        json selected;
        for (const auto& candidate : found.at("candidates")) {
            if (candidate.value("component_id", "") == supplier) selected = candidate;
        }
        REQUIRE(selected.is_object());
        const json arguments = json::array({"--delay-ms", "5"});
        const json options = {{"Threads", 2}, {"Ponder", false}};
        const json registered = control.submit(command("engine.register", {{"kind", "engine"}}, {
            {"display_name", "Selected supplier"}, {"executable", executable.string()},
            {"working_directory", active.string()}, {"arguments", arguments},
            {"uci_options", options}, {"candidate_id", selected.at("candidate_id")}}), "test").at("resource");
        const std::string service_id = evidence.at("resource").at("id");
        const std::string engine_id = registered.at("id");
        const auto submit_target = [&] {
            return control.submit(command("service.evidence_target_register", {{"kind", "managed_service"}, {"id", service_id}},
                {{"engine_id", engine_id}}), "test").at("resource").at("data").at("last_control_result");
        };
        const json result = submit_target();
        REQUIRE(result.at("status") == (capability ? "supported" : "unsupported"));
        const auto enqueue = [&](const std::string& name, const json& configuration, const std::string& target_id = "123e4567-e89b-42d3-a456-426614174001") {
            return control.submit(command("service.evidence_campaign_enqueue", {{"kind", "managed_service"}, {"id", service_id}},
                {{"target_id", target_id}, {"campaign", name}, {"configuration", configuration}}), "test")
                .at("resource").at("data").at("last_control_result");
        };
        REQUIRE(enqueue("example.diagnostic", {{"samples", 4}, {"tag", "test"}}).at("received_configuration") == json{{"samples", 4}, {"tag", "test"}});
        REQUIRE_THROWS_AS(enqueue("unadvertised", json::object()), arena::application::CommandFailure);
        REQUIRE_THROWS_AS(enqueue("example.diagnostic", {{"path", "browser path"}}), arena::application::CommandFailure);
        REQUIRE_THROWS_AS(enqueue("example.diagnostic", {{"samples", 11}}), arena::application::CommandFailure);
        REQUIRE_THROWS_AS(enqueue("example.diagnostic", {{"tag", "too long"}}), arena::application::CommandFailure);
        REQUIRE_THROWS_AS(enqueue("example.diagnostic", json::object(), "123e4567-e89b-42d3-a456-426614174009"), arena::application::CommandFailure);
        if (capability) {
            const json target = result.at("received_target");
            REQUIRE(target.at("schema") == "uci_arena_evidence_target_v2");
            REQUIRE(target.at("launch").at("executable") == registered.at("data").at("executable"));
            REQUIRE(target.at("launch").at("arguments") == arguments);
            REQUIRE(target.at("launch").at("working_directory") == registered.at("data").at("working_directory"));
            REQUIRE(target.at("launch").at("uci_options") == registered.at("data").at("option_overrides"));
            REQUIRE(target.at("runtime_binding").at("component").at("id") == supplier);
            REQUIRE_FALSE(target.contains("model_id"));
            REQUIRE_FALSE(target.contains("provider"));
            // An immutable payload edit must reject the next operation rather than reuse saved capability data.
            std::ofstream(active / "evidence-runtime.json", std::ios::trunc) << "changed";
            REQUIRE_THROWS_AS(submit_target(), arena::application::CommandFailure);
        } else {
            REQUIRE(result.at("created") == false);
            REQUIRE(result.at("code") == "EVIDENCE_RUNTIME_UNSUPPORTED");
            REQUIRE_FALSE(result.contains("target"));
        }
        REQUIRE_THROWS_AS(control.submit(command("service.evidence_target_register", {{"kind", "managed_service"}, {"id", service_id}},
            {{"engine_id", engine_id}, {"target_json", "browser path"}}), "test"), arena::application::CommandFailure);
    }
    fs::remove_all(root);
}

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

TEST_CASE("transient provider launch failure preserves the last validated projection") {
    const fs::path root =
        fs::temp_directory_path() /
        ("arena-service-transient-" + arena::domain::uuid_v7());
    fs::create_directories(root);
    const fs::path node_copy =
        root / fs::path(ARENA_MOCK_SERVICE_PATH).filename();
    fs::copy_file(
        ARENA_MOCK_SERVICE_PATH,
        node_copy,
        fs::copy_options::overwrite_existing);
    {
        auto store = std::make_shared<arena::adapters::SqliteStore>(
            root / "arena.db");
        arena::application::ControlPlane control_plane(store);
        control_plane.initialize();

        const json healthy = register_and_status(
            control_plane,
            root,
            "book_forge",
            "book-transient.mjs",
            node_copy);
        const std::string id = healthy.at("resource").at("id");
        REQUIRE(
            healthy.at("resource").at("data").at("integration_state") ==
            "available");
        REQUIRE(
            healthy.at("resource").at("data").contains("output_integration"));

        fs::remove(node_copy);
        const json unavailable = control_plane.submit(
            command(
                "service.status",
                {{"kind", "managed_service"}, {"id", id}}),
            "test");
        const json& data = unavailable.at("resource").at("data");
        REQUIRE(data.at("integration_state") == "unavailable");
        REQUIRE(data.at("observed_state") == "degraded");
        REQUIRE(data.at("output_projection_stale") == true);
        REQUIRE(data.contains("last_successful_status_at"));
        REQUIRE(data.contains("output_integration"));
    }
    fs::remove_all(root);
}

TEST_CASE("oversized provider output is rejected before JSON interpretation") {
    const fs::path root =
        fs::temp_directory_path() /
        ("arena-service-output-limit-" + arena::domain::uuid_v7());
    fs::create_directories(root);
    {
        auto store = std::make_shared<arena::adapters::SqliteStore>(
            root / "arena.db");
        arena::application::ControlPlane control_plane(store);
        control_plane.initialize();

        const json result = register_and_status(
            control_plane,
            root,
            "book_forge",
            "book-oversized-output.mjs");
        const json& data = result.at("resource").at("data");
        REQUIRE(data.at("integration_state") == "incompatible");
        REQUIRE(data.at("observed_state") == "degraded");
        REQUIRE_FALSE(data.contains("output_integration"));
        REQUIRE(
            data.at("health")
                .at("reason")
                .get<std::string>()
                .find("output exceeded") != std::string::npos);
    }
    fs::remove_all(root);
}

TEST_CASE("provider lifecycle controls await fresh provider truth") {
    const fs::path root =
        fs::temp_directory_path() /
        ("arena-service-control-truth-" + arena::domain::uuid_v7());
    fs::create_directories(root);
    {
        auto store = std::make_shared<arena::adapters::SqliteStore>(
            root / "arena.db");
        arena::application::ControlPlane control_plane(store);
        control_plane.initialize();

        const json healthy = register_and_status(
            control_plane, root, "book_forge", "book-control.mjs");
        const std::string id = healthy.at("resource").at("id");
        const json paused = control_plane.submit(
            command(
                "service.pause",
                {{"kind", "managed_service"}, {"id", id}}),
            "test");
        const json& paused_data = paused.at("resource").at("data");
        REQUIRE(paused_data.at("desired_state") == "paused");
        REQUIRE(paused_data.at("observed_state") == "unknown");
        REQUIRE(
            paused_data.at("last_control_result").at("token") ==
            "[REDACTED]");

        REQUIRE_THROWS_AS(
            control_plane.submit(
                command(
                    "service.resume",
                    {{"kind", "managed_service"}, {"id", id}}),
                "test"),
            arena::application::CommandFailure);
    }
    fs::remove_all(root);
}

TEST_CASE("provider-owned recovery invokes only the declared service API operation") {
    const fs::path root =
        fs::temp_directory_path() /
        ("arena-service-recovery-" + arena::domain::uuid_v7());
    fs::create_directories(root);
    {
        auto store = std::make_shared<arena::adapters::SqliteStore>(
            root / "arena.db");
        arena::application::ControlPlane control_plane(store);
        control_plane.initialize();
        const fs::path cli = root / "book-recovery.mjs";
        const fs::path workspace = root / "book-workspace";
        fs::create_directories(workspace);
        std::ofstream(cli) << "mock";
        const json manager_api{
            {"schema", "uci_arena_manager_service_api_v1"},
            {"version", "1.0"},
            {"transport", "cli_json"},
            {"entrypoint", "cli"},
            {"arguments", {"api"}},
            {"operations",
             {"describe", "status", "outputs_list", "outputs_inspect",
              "config_validate", "startup_recover"}},
            {"output_schema", "uci_arena_service_output_v1"},
        };
        const json data =
            arena::application::ServiceCatalog::normalize_registration({
                {"profile_id", "book_forge"},
                {"node_executable", ARENA_MOCK_SERVICE_PATH},
                {"cli_entrypoint", cli.string()},
                {"workspace", workspace.string()},
                {"startup_installed", true},
                {"installed_component_id", "book_forge.service"},
                {"installed_component_version", "0.4.0"},
                {"manager_integration_api", manager_api},
                {"manager_integration_api_source",
                 "signed_component_manifest"},
            });
        const std::string id = arena::domain::uuid_v7();
        seed_managed_service(
            store, id, data, {{"managed_by", "arena-setup"}});

        const json recovered = control_plane.submit(
            command(
                "service.recover",
                {{"kind", "managed_service"}, {"id", id}}),
            "test");
        const json& recovered_data = recovered.at("resource").at("data");
        REQUIRE(recovered_data.at("desired_state") == "running");
        REQUIRE(recovered_data.at("observed_state") == "starting");
        REQUIRE(
            recovered_data.at("last_control_result") ==
            json{
                {"operation", "startup_recover"},
                {"recovery_requested", true},
                {"registration_mutated", false},
            });
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

TEST_CASE("remembered provider phase is never live runtime authority") {
    const fs::path root =
        fs::temp_directory_path() /
        ("arena-service-live-authority-" + arena::domain::uuid_v7());
    fs::create_directories(root);
    {
        auto store = std::make_shared<arena::adapters::SqliteStore>(
            root / "arena.db");
        arena::application::ControlPlane control_plane(store);
        control_plane.initialize();

        const json dead = register_and_status(
            control_plane, root, "book_forge", "book-dead-phase.mjs");
        const json& dead_data = dead.at("resource").at("data");
        REQUIRE(dead_data.at("integration_state") == "available");
        REQUIRE(dead_data.at("observed_state") == "stopped");
        REQUIRE(dead_data.at("health").at("state") == "healthy");
        REQUIRE(
            dead_data.at("health").at("reason") ==
            "provider reports no live daemon; API contracts validated");

        const std::string dead_id = dead.at("resource").at("id");
        static_cast<void>(control_plane.submit(
            command(
                "resource.remove",
                {{"kind", "managed_service"}, {"id", dead_id}}),
            "test"));

        const json legacy = register_and_status(
            control_plane,
            root,
            "book_forge",
            "book-missing-liveness.mjs");
        const json& legacy_data = legacy.at("resource").at("data");
        REQUIRE(legacy_data.at("integration_state") == "available");
        REQUIRE(legacy_data.at("observed_state") == "stopped");
        REQUIRE(legacy_data.at("health").at("state") == "unknown");
        REQUIRE(
            legacy_data.at("health").at("reason") ==
            "provider status omitted authoritative daemon liveness");
    }
    fs::remove_all(root);
}

TEST_CASE("installed provider API without liveness fails closed to stopped") {
    const fs::path root =
        fs::temp_directory_path() /
        ("arena-service-api-live-authority-" + arena::domain::uuid_v7());
    fs::create_directories(root);
    {
        auto store = std::make_shared<arena::adapters::SqliteStore>(
            root / "arena.db");
        arena::application::ControlPlane control_plane(store);
        control_plane.initialize();

        const json status = register_installed_evidence_and_status(
            control_plane,
            store,
            root,
            "evidence-missing-liveness.mjs");
        const json& data = status.at("resource").at("data");
        REQUIRE(data.at("integration_state") == "available");
        REQUIRE(data.at("observed_state") == "stopped");
        REQUIRE(data.at("health").at("state") == "unknown");
        REQUIRE(
            data.at("health").at("reason") ==
            "provider status omitted authoritative daemon liveness");
        REQUIRE(data.contains("output_integration"));
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
        REQUIRE(
            fs::path(
                book_data.at("output_integration")
                    .at("snapshots")
                    .at(0)
                    .at("path")
                    .get<std::string>())
                .is_absolute());
        REQUIRE(
            fs::path(
                book_data.at("output_integration")
                    .at("snapshots")
                    .at(0)
                    .at("live_channel_root")
                    .get<std::string>())
                .is_absolute());

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
                .at("operations").size() == 10);
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
        const json explicit_campaign = control_plane.submit(
            command(
                "service.evidence_campaign_enqueue",
                {{"kind", "managed_service"}, {"id", evidence_id}},
                {{"target_id", "123e4567-e89b-42d3-a456-426614174001"}}),
            "test");
        REQUIRE(
            explicit_campaign.at("resource").at("data")
                .at("last_control_result").at("target_id") ==
            "123e4567-e89b-42d3-a456-426614174001");
        const json campaign = control_plane.submit(
            command(
                "service.evidence_campaign_enqueue",
                {{"kind", "managed_service"}, {"id", evidence_id}}),
            "test");
        REQUIRE(
            campaign.at("resource").at("data")
                .at("last_control_result").at("campaign") ==
            "time_control_curve_v1");
        REQUIRE(
            campaign.at("resource").at("data")
                .at("last_control_result").at("target_id").is_null());
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

TEST_CASE("cancelled Evidence datasets remain compatible retained evidence") {
    const fs::path root =
        fs::temp_directory_path() /
        ("arena-service-cancelled-dataset-" + arena::domain::uuid_v7());
    fs::create_directories(root);
    {
        auto store = std::make_shared<arena::adapters::SqliteStore>(
            root / "arena.db");
        arena::application::ControlPlane control_plane(store);
        control_plane.initialize();

        const json evidence = register_installed_evidence_and_status(
            control_plane,
            store,
            root,
            "evidence-cancelled-dataset.mjs");
        const json& data = evidence.at("resource").at("data");
        REQUIRE(data.at("integration_state") == "available");
        REQUIRE(data.at("observed_state") == "running");
        const json& dataset =
            data.at("output_integration").at("datasets").at(0);
        REQUIRE(dataset.at("state") == "cancelled");
        REQUIRE(dataset.at("observation_count") == 12);
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
