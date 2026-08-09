#include "arena/adapters/uci_process.hpp"
#include "arena/adapters/sqlite_store.hpp"
#include "arena/application/control_plane.hpp"
#include "arena/application/engine_onboarding.hpp"
#include "arena/domain/uuid.hpp"

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <memory>

#ifndef ARENA_MOCK_UCI_PATH
#error ARENA_MOCK_UCI_PATH must name the mock UCI engine
#endif

TEST_CASE("UCI probe discovers identity options and a legal bestmove") {
    const std::filesystem::path executable = ARENA_MOCK_UCI_PATH;
    const auto result = arena::adapters::UciProcess::probe(
        executable,
        executable.parent_path(),
        {},
        arena::adapters::UciProbeTimeouts{},
        {},
        true);
    REQUIRE(result.ok);
    REQUIRE(result.engine_name == "Arena Mock Engine");
    REQUIRE(result.engine_author == "UCI Arena Manager");
    REQUIRE(result.bestmove == "e2e4");
    REQUIRE(result.options.size() == 3);
    REQUIRE(result.options.front().name == "Threads");
    REQUIRE(result.options.front().type == "spin");
    REQUIRE(result.options.front().minimum == 1);
    REQUIRE(result.options.front().maximum == 16);
    REQUIRE(result.options.at(1).name == "Ponder");
    REQUIRE(result.options.at(1).type == "check");
    REQUIRE(result.fingerprint.size() == 64);
    REQUIRE(result.runtime_model_fingerprint == "8104116015256418347");
    REQUIRE(result.runtime_provider == "cpu");
    REQUIRE(result.runtime_provider_fingerprint == "5805769137532863231");
    REQUIRE(result.runtime_status.has_value());
    REQUIRE(result.runtime_status->model_id == "example_model_v1");
    REQUIRE(result.runtime_status->adapter_id == "dense_planes_v1");
    REQUIRE(result.runtime_status->hardware_driver_fingerprint ==
            "15113055170282736143");
    REQUIRE(result.runtime_status->onnx_runtime_version == "1.24.4");
}

TEST_CASE("UCI probe rejects an option the engine did not advertise") {
    const std::filesystem::path executable = ARENA_MOCK_UCI_PATH;
    const auto result = arena::adapters::UciProcess::probe(
        executable,
        executable.parent_path(),
        {},
        arena::adapters::UciProbeTimeouts{},
        {{"ModelRoot", "not-advertised"}});
    REQUIRE_FALSE(result.ok);
    REQUIRE(
        result.error.find("requested UCI option is not advertised: ModelRoot") !=
        std::string::npos);
}

TEST_CASE("UCI probe gives readiness and first search independent deadlines") {
    using namespace std::chrono_literals;
    const std::filesystem::path executable = ARENA_MOCK_UCI_PATH;
    const arena::adapters::UciProbeTimeouts timeouts{
        2s,
        250ms,
        2s,
        250ms,
    };
    const auto result = arena::adapters::UciProcess::probe(
        executable,
        executable.parent_path(),
        {"--ready-delay-ms", "75", "--delay-ms", "75"},
        timeouts);
    INFO(result.error);
    REQUIRE(result.ok);
    REQUIRE(result.elapsed_ms >= 150);
}

TEST_CASE("engine registration derives preferred metadata from the verified receipt") {
    namespace fs = std::filesystem;
    using nlohmann::json;

    const fs::path root =
        fs::temp_directory_path() /
        ("arena-engine-registration-" + arena::domain::uuid_v7());
    const fs::path data_root = root / "data";
    const fs::path active_root =
        root / "components" / "uci_arena.engine" / "1.2.3";
#ifdef _WIN32
    const fs::path executable =
        active_root / "bin" / "uci_arena_engine.exe";
#else
    const fs::path executable =
        active_root / "bin" / "uci_arena_engine";
#endif
    fs::create_directories(executable.parent_path());
    const fs::path model_root = active_root / "models" / "default";
    const fs::path workspace = root / "workspace" / "uci-arena-engine";
    const fs::path configuration = workspace / "manager-config.json";
    fs::create_directories(model_root);
    fs::create_directories(data_root / "receipts");
    fs::create_directories(workspace);
    fs::copy_file(
        ARENA_MOCK_UCI_PATH,
        executable,
        fs::copy_options::overwrite_existing);
    const auto packaged_probe = arena::adapters::UciProcess::probe(
        executable,
        active_root,
        {},
        arena::adapters::UciProbeTimeouts{});
    REQUIRE(packaged_probe.ok);

    std::ofstream(active_root / "arena-component.json")
        << json{
               {"schema", "arena_install_component_v1"},
               {"schema_version", 1},
               {"component_id", "uci_arena.engine"},
               {"component_version", "1.2.3"},
               {"display_name", "UCI Arena Engine"},
               {"entrypoints",
                {{"uci",
                  fs::relative(executable, active_root).generic_string()}}},
               {"discovery",
                json::array({
                    {
                        {"kind", "uci_engine"},
                        {"locator",
                         {{"source", "entrypoint"},
                          {"entrypoint", "uci"}}},
                    },
                })},
               {"capabilities", {"uci_engine_launch_profile_v1"}},
               {"default_model",
                {{"model_id", "example_model_v1"},
                 {"display_name", "Example Model"},
                 {"input_adapter_id", "dense_planes_v1"},
                 {"root", "models/default"}}},
               {"files",
                json::array({
                    {
                        {"path",
                         fs::relative(executable, active_root)
                             .generic_string()},
                        {"sha256", packaged_probe.fingerprint},
                        {"size_bytes", fs::file_size(executable)},
                    },
                })},
           }
               .dump(2);
    json launch_profile = {
        {"schema", "arena_uci_engine_launch_profile_v1"},
        {"schema_version", 1},
        {"component",
         {{"id", "uci_arena.engine"},
          {"version", "1.2.3"},
          {"root", fs::weakly_canonical(active_root).string()}}},
        {"engine",
         {{"executable", fs::weakly_canonical(executable).string()},
          {"working_directory", fs::weakly_canonical(active_root).string()},
          {"arguments", {"--require-model-root", model_root.string()}}}},
        {"state", "qualified_timing"},
        {"enabled", true},
        {"uci_options",
         {{"ModelRoot", fs::weakly_canonical(model_root).string()},
          {"Threads", 2},
          {"Ponder", false}}},
        {"expected_runtime",
         {{"timing_contract_version",
           "persistent_mcgs_relative_clock_temporal_v4"},
          {"model_id", "example_model_v1"},
          {"model_fingerprint", "8104116015256418347"},
          {"adapter_id", "dense_planes_v1"},
          {"provider_requested", "cpu"},
          {"provider_effective", "cpu"},
          {"provider_numeric_fingerprint", "5805769137532863231"},
          {"hardware_driver_fingerprint", "15113055170282736143"},
          {"onnx_runtime_version", "1.24.4"},
          {"search_core", "mcgs"}}},
        {"evidence",
         {{"timing_profile",
           {{"schema", "uci_time_profile_v3"},
            {"identity_sha256", "sha256:fixture"}}}}},
        {"knowledge", json::object()},
        {"diagnostics", json::array()},
    };
    std::ofstream(configuration) << launch_profile.dump(2);
    std::ofstream(data_root / "receipts" / "uci_arena.engine.json")
        << json{
               {"schema", "arena_install_receipt_v1"},
               {"schema_version", 1},
               {"component_id", "uci_arena.engine"},
               {"component_version", "1.2.3"},
               {"state", "installed_managed"},
               {"active_root", active_root.string()},
               {"artifact_sha256", std::string(64, 'a')},
               {"data_paths", {workspace.string()}},
               {"configuration_path", configuration.string()},
           }
               .dump(2);

    const json discovery =
        arena::application::EngineOnboarding::discover(
            data_root, {}, false);
    REQUIRE(discovery.at("candidates").size() == 1);
    const std::string candidate_id =
        discovery.at("candidates").at(0).at("candidate_id");
    const json candidate_options =
        discovery.at("candidates").at(0).at("uci_options");
    REQUIRE(
        fs::path(candidate_options.at("ModelRoot").get<std::string>()) ==
        fs::weakly_canonical(model_root));

    {
        auto store =
            std::make_shared<arena::adapters::SqliteStore>(
                root / "arena.db");
        arena::application::ControlPlane control_plane(store);
        control_plane.initialize();
        control_plane.reconcile_installed_services(data_root);

        const auto registration =
            [&](const std::string& selected_candidate_id) {
                return control_plane.submit(
                {
                    {"schema_version", 1},
                    {"request_id", arena::domain::uuid_v7()},
                    {"idempotency_key", arena::domain::uuid_v7()},
                    {"target",
                     {{"kind", "engine"}, {"id", nullptr}}},
                    {"action", "engine.register"},
                    {"expected_revision", nullptr},
                    {"dry_run", false},
                    {"parameters",
                     {
                         {"display_name", "UCI Arena Engine"},
                         {"executable", executable.string()},
                         {"working_directory",
                          active_root.string()},
                         {"arguments",
                          {"--require-model-root", model_root.string()}},
                         {"uci_options", candidate_options},
                         {"candidate_id",
                          selected_candidate_id},
                         {"component_id", "forged.engine"},
                         {"component_version", "99.0.0"},
                         {"recommended", false},
                         {"default_model",
                          {{"model_id", "forged"},
                           {"display_name", "Forged"},
                           {"input_adapter_id", "forged"}}},
                     }},
                    },
                    "test");
            };

        const json registered = registration(candidate_id).at("resource");
        const json verified = registered.at("data");
        REQUIRE(verified.at("candidate_id") == candidate_id);
        REQUIRE(
            verified.at("onboarding_source") ==
            "install_receipt");
        REQUIRE(
            verified.at("component_id") == "uci_arena.engine");
        REQUIRE(verified.at("component_version") == "1.2.3");
        REQUIRE(verified.at("recommended") == true);
        REQUIRE(
            verified.at("default_model").at("display_name") ==
            "Example Model");
        REQUIRE(
            fs::path(
                verified.at("option_overrides")
                    .at("ModelRoot")
                    .get<std::string>()) == fs::weakly_canonical(model_root));
        REQUIRE(verified.at("option_overrides").at("Threads") == "2");
        REQUIRE(verified.at("option_overrides").at("Ponder") == "false");
        REQUIRE(
            verified.at("runtime_model_identity")
                .at("model_fingerprint") == "8104116015256418347");

        const json forged_resource =
            registration("engcand-forged").at("resource");
        const json forged = forged_resource.at("data");
        REQUIRE_FALSE(forged.contains("candidate_id"));
        REQUIRE_FALSE(forged.contains("component_id"));
        REQUIRE_FALSE(forged.contains("component_version"));
        REQUIRE_FALSE(forged.contains("recommended"));
        REQUIRE_FALSE(forged.contains("default_model"));
        static_cast<void>(control_plane.submit(
            {
                {"schema_version", 1},
                {"request_id", arena::domain::uuid_v7()},
                {"idempotency_key", arena::domain::uuid_v7()},
                {"target",
                 {{"kind", "engine"}, {"id", forged_resource.at("id")}}},
                {"action", "resource.remove"},
                {"expected_revision", forged_resource.at("revision")},
                {"dry_run", false},
                {"parameters", json::object()},
            },
            "test"));
        REQUIRE(fs::is_regular_file(executable));

        launch_profile["uci_options"]["Threads"] = 2;
        std::ofstream(configuration, std::ios::trunc)
            << launch_profile.dump(2);
        const json refreshed_candidate =
            arena::application::EngineOnboarding::discover(
                data_root, {}, false)
                .at("candidates")
                .at(0);
        const auto refresh = [&](const std::uint64_t revision) {
            return control_plane.submit(
                {
                    {"schema_version", 1},
                    {"request_id", arena::domain::uuid_v7()},
                    {"idempotency_key", arena::domain::uuid_v7()},
                    {"target",
                     {{"kind", "engine"}, {"id", registered.at("id")}}},
                    {"action", "engine.refresh"},
                    {"expected_revision", revision},
                    {"dry_run", false},
                    {"parameters",
                     {
                         {"display_name", "UCI Arena Engine"},
                         {"executable", refreshed_candidate.at("executable")},
                         {"working_directory",
                          refreshed_candidate.at("working_directory")},
                         {"arguments", refreshed_candidate.at("arguments")},
                         {"uci_options", refreshed_candidate.at("uci_options")},
                         {"candidate_id", refreshed_candidate.at("candidate_id")},
                     }},
                },
                "test");
        };
        const json refreshed = refresh(registered.at("revision")).at("resource");
        REQUIRE(refreshed.at("id") == registered.at("id"));
        REQUIRE(refreshed.at("revision") == 2);
        REQUIRE(refreshed.at("data").at("option_overrides").at("Threads") == "2");

        auto prior_version = store->get_resource(
            "engine", registered.at("id").get<std::string>()).value();
        json prior_version_data = json::parse(prior_version.data_json);
        prior_version_data["candidate_id"] = "engcand-prior-immutable-root";
        prior_version_data["component_version"] = "1.2.2";
        prior_version.revision += 1;
        prior_version.updated_at = arena::domain::utc_now();
        prior_version.data_json = prior_version_data.dump();
        const std::string migration_command_id = arena::domain::uuid_v7();
        store->update_resource(
            prior_version,
            refreshed.at("revision"),
            {
                migration_command_id,
                arena::domain::uuid_v7(),
                arena::domain::uuid_v7(),
                "test.prior-engine-version",
                "completed",
                "{}",
                prior_version.updated_at,
            },
            {
                0,
                arena::domain::uuid_v7(),
                prior_version.updated_at,
                "state",
                "engine.test_prior_version",
                "engine",
                prior_version.id,
                prior_version.revision,
                migration_command_id,
                "{}",
            });
        const json version_refreshed =
            refresh(prior_version.revision).at("resource");
        REQUIRE(version_refreshed.at("id") == registered.at("id"));
        REQUIRE(version_refreshed.at("revision") == 4);
        REQUIRE(
            version_refreshed.at("data").at("candidate_id") == candidate_id);
        REQUIRE(
            version_refreshed.at("data").at("component_version") == "1.2.3");

        launch_profile["expected_runtime"]["hardware_driver_fingerprint"] =
            "15113055170282736144";
        std::ofstream(configuration, std::ios::trunc)
            << launch_profile.dump(2);
        REQUIRE_THROWS(refresh(version_refreshed.at("revision")));
        json after_failed_refresh = nullptr;
        for (const auto& engine : control_plane.list("engine")) {
            if (engine.at("id") == registered.at("id")) {
                after_failed_refresh = engine;
                break;
            }
        }
        REQUIRE_FALSE(after_failed_refresh.is_null());
        REQUIRE(
            after_failed_refresh.at("revision") ==
            version_refreshed.at("revision"));
        REQUIRE(
            after_failed_refresh.at("data").at("launch_profile") ==
            version_refreshed.at("data").at("launch_profile"));

        launch_profile["expected_runtime"]["hardware_driver_fingerprint"] =
            "15113055170282736143";
        std::ofstream(configuration, std::ios::trunc)
            << launch_profile.dump(2);
        const std::string now = arena::domain::utc_now();
        const std::string competition_id = arena::domain::uuid_v7();
        const std::string command_id = arena::domain::uuid_v7();
        store->create_resource(
            {
                competition_id,
                "match_plan",
                "local",
                1,
                now,
                now,
                "{}",
                json{
                    {"lifecycle", "paused"},
                    {"engine_ids", {registered.at("id")}},
                }.dump(),
            },
            {
                command_id,
                arena::domain::uuid_v7(),
                arena::domain::uuid_v7(),
                "match.plan",
                "completed",
                "{}",
                now,
            },
            {
                0,
                arena::domain::uuid_v7(),
                now,
                "state",
                "match_plan.created",
                "match_plan",
                competition_id,
                1,
                command_id,
                "{}",
            });
        REQUIRE_THROWS(refresh(version_refreshed.at("revision")));
        REQUIRE_THROWS_AS(
            control_plane.submit(
                {
                    {"schema_version", 1},
                    {"request_id", arena::domain::uuid_v7()},
                    {"idempotency_key", arena::domain::uuid_v7()},
                    {"target",
                     {{"kind", "engine"}, {"id", registered.at("id")}}},
                    {"action", "resource.remove"},
                    {"expected_revision", version_refreshed.at("revision")},
                    {"dry_run", false},
                    {"parameters", json::object()},
                },
                "test"),
            arena::application::CommandFailure);
    }

    fs::remove_all(root);
}
