#include "arena/adapters/uci_process.hpp"
#include "arena/adapters/sqlite_store.hpp"
#include "arena/application/control_plane.hpp"
#include "arena/application/engine_onboarding.hpp"
#include "arena/domain/uuid.hpp"

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <algorithm>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <memory>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

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

TEST_CASE("timed-only UCI engine passes preview and fresh registration without depth evidence") {
    namespace fs = std::filesystem;
    using nlohmann::json;
    const fs::path executable = ARENA_MOCK_UCI_PATH;
    const auto result = arena::adapters::UciProcess::probe(
        executable, executable.parent_path(), {"--publication-only"},
        arena::adapters::UciProbeTimeouts{}, {{"Threads", "2"}});
    INFO(result.error);
    REQUIRE(result.ok);
    REQUIRE(result.bestmove == "e2e4");
    REQUIRE(result.options.size() == 3);

    const fs::path root = fs::temp_directory_path() /
        ("arena-timed-registration-" + arena::domain::uuid_v7());
    fs::create_directories(root);
    {
        auto store = std::make_shared<arena::adapters::SqliteStore>(root / "arena.db");
        arena::application::ControlPlane control(store);
        control.initialize();
        const auto register_engine = [&](bool dry_run, bool illegal_move = false) {
            const json arguments = illegal_move
                ? json::array({"--publication-only", "--illegal"})
                : json::array({"--publication-only"});
            return control.submit(
                {{"schema_version", 1},
                 {"request_id", arena::domain::uuid_v7()},
                 {"idempotency_key", arena::domain::uuid_v7()},
                 {"target", {{"kind", "engine"}, {"id", nullptr}}},
                 {"action", "engine.register"},
                 {"expected_revision", nullptr},
                 {"dry_run", dry_run},
                 {"parameters", {{"display_name", "Publication-only peer"},
                                 {"executable", executable.string()},
                                 {"working_directory", executable.parent_path().string()},
                                 {"arguments", arguments},
                                 {"uci_options", {{"Threads", 2}}}}}}, "test");
        };
        REQUIRE(control.list("engine").empty());
        const json preview = register_engine(true);
        REQUIRE(preview.at("dry_run") == true);
        REQUIRE(control.list("engine").empty());
        REQUIRE_THROWS_AS(register_engine(false, true), arena::application::CommandFailure);
        REQUIRE(control.list("engine").empty());
        const json registered = register_engine(false).at("resource");
        REQUIRE(control.list("engine").size() == 1);
        REQUIRE(registered.at("data").at("fingerprint") == result.fingerprint);
        REQUIRE(registered.at("data").at("option_overrides").at("Threads") == "2");
        REQUIRE(registered.at("data").at("arguments") == json::array({"--publication-only"}));
    }
    fs::remove_all(root);
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

TEST_CASE("receipt engine registration and explicit refresh preserve verified identity and options") {
    namespace fs = std::filesystem;
    using nlohmann::json;
    const std::string component_id = GENERATE(
        "uci_arena.engine", "uci_arena.vector", "example.alternate-engine");
    INFO("registered component: " << component_id);

    const fs::path root =
        fs::temp_directory_path() /
        ("arena-engine-registration-" + arena::domain::uuid_v7());
    const fs::path data_root = root / "data";
    const fs::path active_root =
        root / "components" / component_id / "1.2.3";
    const fs::path receipt_path = data_root / "receipts" / (component_id + ".json");
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
               {"component_id", component_id},
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
         {{"id", component_id},
          {"version", "1.2.3"},
          {"root", fs::weakly_canonical(active_root).string()}}},
        {"engine",
         {{"executable", fs::weakly_canonical(executable).string()},
          {"working_directory", fs::weakly_canonical(active_root).string()},
          {"arguments", {"--allow-model-root"}}}},
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
    fs::path receipt_active_root = active_root;
#ifdef _WIN32
    const std::string receipt_spelling = GENERATE("canonical", "case_alias", "short_alias");
    INFO("receipt path spelling: " << receipt_spelling);
    if (receipt_spelling == "case_alias") {
        std::wstring alias = active_root.native();
        std::transform(alias.begin(), alias.end(), alias.begin(), [](const wchar_t value) {
            return static_cast<wchar_t>(std::towupper(value));
        });
        receipt_active_root = alias;
    } else if (receipt_spelling == "short_alias") {
        const DWORD required = GetShortPathNameW(active_root.c_str(), nullptr, 0);
        REQUIRE(required > 0);
        std::wstring alias(required, L'\0');
        const DWORD written = GetShortPathNameW(active_root.c_str(), alias.data(), required);
        REQUIRE(written > 0);
        REQUIRE(written < required);
        alias.resize(written);
        receipt_active_root = alias;
        if (receipt_active_root == active_root) {
            WARN("8.3 aliases are unavailable; the case-alias run still covers equivalent spelling");
        }
    }
    REQUIRE(fs::equivalent(receipt_active_root, active_root));
#endif
    std::ofstream(receipt_path)
        << json{
               {"schema", "arena_install_receipt_v1"},
               {"schema_version", 1},
               {"component_id", component_id},
               {"component_version", "1.2.3"},
               {"state", "installed_managed"},
               {"active_root", receipt_active_root.string()},
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
    REQUIRE(discovery.at("candidates")
                .at(0)
                .at("installed_artifact_sha256") == std::string(64, 'a'));
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
            [&](const std::string& selected_candidate_id,
                const json& selected_options,
                const bool dry_run,
                const fs::path& requested_executable = {},
                arena::application::ControlPlane* requested_authority = nullptr) {
                json request{
                    {"schema_version", 1},
                    {"request_id", arena::domain::uuid_v7()},
                    {"idempotency_key", arena::domain::uuid_v7()},
                    {"target",
                     {{"kind", "engine"}, {"id", nullptr}}},
                    {"action", "engine.register"},
                    {"expected_revision", nullptr},
                    {"dry_run", dry_run},
                    {"parameters",
                     {
                         {"display_name", "UCI Arena Engine"},
                         {"executable", requested_executable.empty()
                             ? executable.string() : requested_executable.string()},
                         {"working_directory",
                          active_root.string()},
                         {"arguments", {"--allow-model-root"}},
                         {"uci_options", selected_options},
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
                };
                if (selected_candidate_id.empty()) {
                    request["parameters"].erase("candidate_id");
                }
                return (requested_authority ? *requested_authority : control_plane)
                    .submit(request, "test");
            };

        const json registered =
            registration(candidate_id, candidate_options, false).at("resource");
        const json verified = registered.at("data");
        REQUIRE(verified.at("candidate_id") == candidate_id);
        REQUIRE(
            verified.at("onboarding_source") ==
            "install_receipt");
        REQUIRE(
            verified.at("component_id") == component_id);
        REQUIRE(verified.at("component_version") == "1.2.3");
        REQUIRE(verified.at("installed_artifact_sha256") ==
                std::string(64, 'a'));
        REQUIRE(verified.at("model_selection_authority") ==
                "packaged_launch_profile");
        REQUIRE(verified.at("recommended") == (component_id == "uci_arena.engine"));
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

        const fs::path different_executable = root / executable.filename();
        fs::copy_file(ARENA_MOCK_UCI_PATH, different_executable);
        REQUIRE_FALSE(fs::equivalent(different_executable, executable));
        auto raw_store = std::make_shared<arena::adapters::SqliteStore>(root / "raw.db");
        arena::application::ControlPlane without_inventory(raw_store);
        without_inventory.initialize();
        const auto reject_selector = [&](const std::string& selector,
                                         const fs::path& selected_executable,
                                         arena::application::ControlPlane* authority,
                                         const bool dry_run) {
            try {
                static_cast<void>(registration(selector, candidate_options, dry_run,
                                               selected_executable, authority));
                FAIL("an unresolved explicit selector must not become raw launch authority");
            } catch (const arena::application::CommandFailure& error) {
                REQUIRE(error.code() == "engine_candidate_unavailable");
            }
            REQUIRE(control_plane.list("engine").size() == 1);
            REQUIRE(store->get_resource("engine", registered.at("id").get<std::string>())
                        ->revision == registered.at("revision"));
            REQUIRE(without_inventory.list("engine").empty());
        };
        for (const bool dry_run : {true, false}) {
            reject_selector("engcand-stale", executable, nullptr, dry_run);
            reject_selector(candidate_id, different_executable, nullptr, dry_run);
            reject_selector(candidate_id, executable, &without_inventory, dry_run);
        }
        const json raw_preview = registration("", candidate_options, true, executable,
                                              &without_inventory).at("resource").at("data");
        REQUIRE_FALSE(raw_preview.contains("candidate_id"));
        REQUIRE_FALSE(raw_preview.contains("component_id"));
        REQUIRE(without_inventory.list("engine").empty());
        const json raw_registered = registration("", candidate_options, false, executable,
                                                 &without_inventory).at("resource").at("data");
        REQUIRE_FALSE(raw_registered.contains("candidate_id"));
        REQUIRE_FALSE(raw_registered.contains("component_id"));
        REQUIRE(without_inventory.list("engine").size() == 1);

        json invalid_profile = launch_profile;
        invalid_profile["component"]["version"] = "9.9.9";
        std::ofstream(configuration, std::ios::trunc) << invalid_profile.dump(2);
        const json unavailable = control_plane.engine_candidates().at("candidates").at(0);
        REQUIRE(unavailable.at("candidate_id") == candidate_id);
        REQUIRE(unavailable.at("state") == "unavailable");
        REQUIRE_FALSE(unavailable.contains("launch_profile"));
        for (const bool dry_run : {true, false}) {
            try {
                static_cast<void>(registration(candidate_id, candidate_options, dry_run));
                FAIL("an invalid declared launch profile must not become raw launch authority");
            } catch (const arena::application::CommandFailure& error) {
                REQUIRE(error.code() == "engine_candidate_unavailable");
            }
            REQUIRE(control_plane.list("engine").size() == 1);
            REQUIRE(store->get_resource("engine", registered.at("id").get<std::string>())
                        ->revision == registered.at("revision"));
        }
        std::ofstream(configuration, std::ios::trunc) << launch_profile.dump(2);

        const fs::path alternate_model_root =
            root / "operator-models" / "operator-selected";
        fs::create_directories(alternate_model_root);
        json selected_options = candidate_options;
        selected_options["ModelRoot"] = alternate_model_root.string();
        const json selected_preview =
            registration(candidate_id, selected_options, true)
                .at("resource")
                .at("data");
        REQUIRE(
            fs::path(
                selected_preview.at("option_overrides")
                    .at("ModelRoot")
                    .get<std::string>()) == alternate_model_root);
        REQUIRE_FALSE(selected_preview.contains("launch_profile"));
        REQUIRE(selected_preview.at("operational_timing_binding")
                    .at("schema") ==
                "arena_operational_timing_binding_v1");
        REQUIRE(selected_preview.at("operational_timing_binding")
                    .at("timing_profile")
                    .at("identity_sha256") == "sha256:fixture");
        REQUIRE(selected_preview.at("operational_timing_binding")
                    .at("operational_distribution") == "retained");
        REQUIRE(selected_preview.at("operational_timing_binding")
                    .at("marginal_move_quality") == "unqualified");
        REQUIRE(
            selected_preview.at("runtime_model_identity")
                .at("model_fingerprint") == "8104116015256418347");
        REQUIRE(
            selected_preview.at("default_model").at("display_name") ==
            "Example Model");

        const json selected_refresh_preview =
            control_plane.submit(
                {
                    {"schema_version", 1},
                    {"request_id", arena::domain::uuid_v7()},
                    {"idempotency_key", arena::domain::uuid_v7()},
                    {"target",
                     {{"kind", "engine"}, {"id", registered.at("id")}}},
                    {"action", "engine.refresh"},
                    {"expected_revision", registered.at("revision")},
                    {"dry_run", true},
                    {"parameters",
                     {
                         {"display_name", "UCI Arena Engine"},
                         {"executable", executable.string()},
                         {"working_directory", active_root.string()},
                         {"arguments", {"--allow-model-root"}},
                         {"uci_options", selected_options},
                         {"candidate_id", candidate_id},
                     }},
                },
                "test")
                .at("resource");
        REQUIRE(selected_refresh_preview.at("id") == registered.at("id"));
        REQUIRE_FALSE(
            selected_refresh_preview.at("data").contains("launch_profile"));
        REQUIRE(selected_refresh_preview.at("data")
                    .at("operational_timing_binding")
                    .at("timing_profile")
                    .at("identity_sha256") == "sha256:fixture");
        REQUIRE(selected_refresh_preview.at("data")
                    .at("model_selection_authority") == "operator");

        json changed_profile_options = selected_options;
        changed_profile_options["Threads"] = 3;
        REQUIRE_THROWS(
            registration(candidate_id, changed_profile_options, true));

        for (const bool dry_run : {true, false}) {
            try {
                static_cast<void>(registration("engcand-forged", candidate_options, dry_run));
                FAIL("a forged explicit selector must not become raw launch authority");
            } catch (const arena::application::CommandFailure& error) {
                REQUIRE(error.code() == "engine_candidate_unavailable");
            }
            REQUIRE(control_plane.list("engine").size() == 1);
            REQUIRE(store->get_resource("engine", registered.at("id").get<std::string>())
                        ->revision == registered.at("revision"));
        }
        const json forged_resource =
            registration("", candidate_options, false)
                .at("resource");
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

        // Automatic startup reconciliation remains the existing suite-default
        // policy. Explicit receipt refresh above applies to every component.
        if (component_id == "uci_arena.engine") {
        json replacement_receipt = json::parse(std::ifstream(receipt_path));
        // A busy Engine is preserved even while receipt replacement makes
        // its next installed candidate temporarily unavailable.
        fs::remove(receipt_path);
        control_plane.reconcile_installed_services(data_root);
        REQUIRE(control_plane.status()
                    .at("installed_engine_reconciliation")
                    .at("outcome") == "active_use_preserved");
        replacement_receipt["artifact_sha256"] = std::string(64, 'b');
        std::ofstream(receipt_path,
                      std::ios::trunc)
            << replacement_receipt.dump(2);
        control_plane.reconcile_installed_services(data_root);
        REQUIRE(control_plane.status()
                    .at("installed_engine_reconciliation")
                    .at("outcome") == "active_use_preserved");
        REQUIRE(store->get_resource(
                    "engine", registered.at("id").get<std::string>())
                    ->revision == version_refreshed.at("revision"));

        auto completed_competition =
            store->get_resource("match_plan", competition_id).value();
        json completed_competition_data =
            json::parse(completed_competition.data_json);
        completed_competition_data["lifecycle"] = "completed";
        completed_competition.revision += 1;
        completed_competition.updated_at = arena::domain::utc_now();
        completed_competition.data_json = completed_competition_data.dump();
        const std::string completion_command_id = arena::domain::uuid_v7();
        store->update_resource(
            completed_competition,
            1,
            {
                completion_command_id,
                arena::domain::uuid_v7(),
                arena::domain::uuid_v7(),
                "test.complete-competition",
                "completed",
                "{}",
                completed_competition.updated_at,
            },
            {
                0,
                arena::domain::uuid_v7(),
                completed_competition.updated_at,
                "state",
                "match_plan.completed",
                "match_plan",
                competition_id,
                completed_competition.revision,
                completion_command_id,
                "{}",
            });
        control_plane.reconcile_installed_services(data_root);
        const json reconciliation =
            control_plane.status().at("installed_engine_reconciliation");
        REQUIRE(reconciliation.at("outcome") == "refreshed");
        auto automatically_refreshed = store->get_resource(
            "engine", registered.at("id").get<std::string>()).value();
        REQUIRE(automatically_refreshed.revision == 5);
        REQUIRE(json::parse(automatically_refreshed.data_json)
                    .at("installed_artifact_sha256") == std::string(64, 'b'));

        const json operator_selected = control_plane.submit(
            {
                {"schema_version", 1},
                {"request_id", arena::domain::uuid_v7()},
                {"idempotency_key", arena::domain::uuid_v7()},
                {"target",
                 {{"kind", "engine"}, {"id", registered.at("id")}}},
                {"action", "engine.refresh"},
                {"expected_revision", automatically_refreshed.revision},
                {"dry_run", false},
                {"parameters",
                 {
                     {"display_name", "UCI Arena Engine"},
                     {"executable", executable.string()},
                     {"working_directory", active_root.string()},
                     {"arguments", {"--allow-model-root"}},
                     {"uci_options", selected_options},
                     {"candidate_id", candidate_id},
                 }},
            },
            "test").at("resource");
        REQUIRE(operator_selected.at("data")
                    .at("model_selection_authority") == "operator");
        REQUIRE_FALSE(operator_selected.at("data").contains("launch_profile"));
        REQUIRE(operator_selected.at("data")
                    .at("operational_timing_binding")
                    .at("operational_distribution") == "retained");
        REQUIRE(operator_selected.at("data")
                    .at("operational_timing_binding")
                    .at("marginal_move_quality") == "unqualified");

        replacement_receipt["artifact_sha256"] = std::string(64, 'c');
        std::ofstream(receipt_path,
                      std::ios::trunc)
            << replacement_receipt.dump(2);
        control_plane.reconcile_installed_services(data_root);
        REQUIRE(control_plane.status()
                    .at("installed_engine_reconciliation")
                    .at("outcome") == "operator_model_override_preserved");
        const auto preserved_operator = store->get_resource(
            "engine", registered.at("id").get<std::string>()).value();
        REQUIRE(preserved_operator.revision ==
                operator_selected.at("revision"));
        REQUIRE(fs::path(json::parse(preserved_operator.data_json)
                             .at("option_overrides")
                             .at("ModelRoot")
                             .get<std::string>()) == alternate_model_root);
        }
    }

    fs::remove_all(root);
}
