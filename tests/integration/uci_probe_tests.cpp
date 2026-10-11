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

TEST_CASE("receipt clock binding uses actual local probe control before readiness") {
    namespace fs = std::filesystem;
    using nlohmann::json;
    const fs::path root = fs::temp_directory_path() / ("arena-clock-probe-" + arena::domain::uuid_v7());
    const fs::path data = root / "data", active = root / "component", workspace = root / "workspace";
    const fs::path executable = active / fs::path(ARENA_MOCK_UCI_PATH).filename();
    const fs::path configuration = workspace / "profile.json", wire = root / "wire.log";
    fs::create_directories(active);
    fs::create_directories(data / "receipts");
    fs::create_directories(workspace);
    fs::copy_file(ARENA_MOCK_UCI_PATH, executable);
    const auto fingerprint = arena::adapters::UciProcess::probe(executable, active, {}).fingerprint;
    const json binding = {{"schema", "arena_uci_clock_option_bindings_v1"}, {"schema_version", 1},
                          {"bindings", {{"InitialBudgetMs", "initial_ms"}}}};
    const json arguments = {"--clock-option", "InitialBudgetMs", "--expected-initial-ms", "180000",
                            "--expected-increment-ms", "0", "--wait-for-stop", "--wire-log", wire.string()};
    const json options = {{"Threads", 2}, {"InitialBudgetMs", 0}};
    const json profile = {
        {"schema", "arena_uci_engine_launch_profile_v1"}, {"schema_version", 1},
        {"component", {{"id", "uci_arena.engine"}, {"version", "1.0.0"}, {"root", fs::weakly_canonical(active).string()}}},
        {"engine", {{"executable", fs::weakly_canonical(executable).string()}, {"working_directory", fs::weakly_canonical(active).string()}, {"arguments", arguments}}},
        {"state", "conservative"}, {"enabled", true}, {"uci_options", options},
        {"clock_option_bindings", binding}, {"expected_runtime", nullptr}, {"evidence", json::object()},
        {"knowledge", json::object()}, {"diagnostics", json::array()},
    };
    std::ofstream(configuration) << profile.dump();
    std::ofstream(active / "arena-component.json") << json{
        {"schema", "arena_install_component_v1"}, {"schema_version", 1},
        {"component_id", "uci_arena.engine"}, {"component_version", "1.0.0"},
        {"entrypoints", {{"uci", executable.filename().string()}}},
        {"discovery", json::array({{{"kind", "uci_engine"}, {"locator", {{"source", "entrypoint"}, {"entrypoint", "uci"}}}}})},
        {"capabilities", {"uci_engine_launch_profile_v1"}},
        {"files", json::array({{{"path", executable.filename().string()}, {"sha256", fingerprint}, {"size_bytes", fs::file_size(executable)}}})},
    }.dump();
    std::ofstream(data / "receipts" / "uci_arena.engine.json") << json{
        {"schema", "arena_install_receipt_v1"}, {"schema_version", 1}, {"component_id", "uci_arena.engine"},
        {"component_version", "1.0.0"}, {"state", "installed_managed"}, {"active_root", active.string()},
        {"artifact_sha256", std::string(64, 'a')}, {"data_paths", {workspace.string()}}, {"configuration_path", configuration.string()},
    }.dump();
    {
        auto store = std::make_shared<arena::adapters::SqliteStore>(root / "arena.db");
        arena::application::ControlPlane control(store);
        control.initialize();
        control.reconcile_installed_services(data);
        const auto candidate = control.engine_candidates().at("candidates").at(0);
        REQUIRE(candidate.at("state") == "ready_to_probe");
        json parameters = {{"display_name", "Clock peer"}, {"executable", executable.string()},
                           {"working_directory", active.string()}, {"arguments", arguments},
                           {"uci_options", options}, {"candidate_id", candidate.at("candidate_id")},
                           {"time_control", {{"initial_ms", 180000}, {"increment_ms", 0}}}};
        const auto submit = [&] (bool dry_run) {
            return control.submit({{"schema_version", 1}, {"request_id", arena::domain::uuid_v7()},
                {"idempotency_key", arena::domain::uuid_v7()}, {"target", {{"kind", "engine"}, {"id", nullptr}}},
                {"action", "engine.register"}, {"expected_revision", nullptr}, {"dry_run", dry_run},
                {"parameters", parameters}}, "test");
        };
        SECTION("actual clock binds declared names and stop consumes the completed move") {
            const auto preview = submit(true).at("resource").at("data");
            REQUIRE(control.list("engine").empty());
            REQUIRE(preview.at("option_overrides").at("InitialBudgetMs") == "0");
            REQUIRE(preview.at("clock_option_bindings") == binding);
            REQUIRE(preview.at("probe_time_control") == parameters.at("time_control"));
            REQUIRE(preview.at("probe_bestmove") == "e2e4");
            const auto registered = submit(false).at("resource").at("data");
            REQUIRE(registered.at("probe_time_control") == parameters.at("time_control"));
            const std::string transcript{std::istreambuf_iterator<char>(std::ifstream(wire).rdbuf()), {}};
            const auto new_game = transcript.find("ucinewgame\n");
            const auto binding_line = transcript.find("setoption name InitialBudgetMs value 180000\n");
            REQUIRE(new_game < binding_line);
            REQUIRE(binding_line < transcript.find("isready\n"));
            REQUIRE(transcript.find("go wtime 180000 btime 180000 winc 0 binc 0\nstop\n") != std::string::npos);
            REQUIRE(transcript.find("go movetime") == std::string::npos);
            REQUIRE(transcript.ends_with("quit\nowned-child-exit\n"));
            REQUIRE(preview.at("probe_elapsed_ms").get<unsigned>() >= 500);
        }
        SECTION("an early completed move avoids stop and joins the child") {
            auto early_arguments = arguments;
            early_arguments.erase(std::find(early_arguments.begin(), early_arguments.end(), "--wait-for-stop"));
            auto early_profile = profile;
            early_profile["engine"]["arguments"] = early_arguments;
            std::ofstream(configuration, std::ios::trunc) << early_profile.dump();
            const auto early_candidate = control.engine_candidates().at("candidates").at(0);
            parameters["candidate_id"] = early_candidate.at("candidate_id");
            parameters["arguments"] = early_arguments;
            const auto preview = submit(true).at("resource").at("data");
            REQUIRE(preview.at("probe_bestmove") == "e2e4");
            const std::string transcript{std::istreambuf_iterator<char>(std::ifstream(wire).rdbuf()), {}};
            REQUIRE(transcript.find("go wtime 180000 btime 180000 winc 0 binc 0\n") != std::string::npos);
            REQUIRE(transcript.find("stop\n") == std::string::npos);
            REQUIRE(transcript.ends_with("quit\nowned-child-exit\n"));
        }
        SECTION("missing original control fails before any child launch") {
            parameters.erase("time_control");
            try { static_cast<void>(submit(true)); FAIL("missing actual probe control was admitted"); }
            catch (const arena::application::CommandFailure& error) { REQUIRE(error.code() == "engine_clock_required"); }
            REQUIRE_FALSE(fs::exists(wire));
            REQUIRE(control.list("engine").empty());
        }
        SECTION("unknown original control fails before any child launch") {
            parameters["time_control"]["initial_ms"] = 0;
            try { static_cast<void>(submit(true)); FAIL("unknown actual probe control was admitted"); }
            catch (const arena::application::CommandFailure& error) { REQUIRE(error.code() == "engine_clock_invalid"); }
            REQUIRE_FALSE(fs::exists(wire));
        }
        SECTION("unbound profile changes are rejected before launch") {
            parameters["uci_options"]["Threads"] = 7;
            REQUIRE_THROWS_AS(submit(true), arena::application::CommandFailure);
            REQUIRE_FALSE(fs::exists(wire));
        }
        SECTION("startup preserves registration and requires a new explicit control when declaration changes") {
            const auto registered = submit(false).at("resource");
            fs::remove(wire);
            auto replacement = profile;
            replacement["clock_option_bindings"]["bindings"]["InitialBudgetMs"] = "increment_ms";
            std::ofstream(configuration, std::ios::trunc) << replacement.dump();
            control.reconcile_installed_services(data);
            REQUIRE(control.status().at("installed_engine_reconciliation").at("outcome") == "explicit_probe_clock_required");
            REQUIRE_FALSE(fs::exists(wire));
            REQUIRE(control.list("engine").at(0).at("revision") == registered.at("revision"));
        }
    }
    fs::remove_all(root);
}

TEST_CASE("clock probe rejects non-spin and incompatible original values before readiness and joins") {
    namespace fs = std::filesystem;
    const fs::path root = fs::temp_directory_path() / ("arena-clock-advertisement-" + arena::domain::uuid_v7());
    fs::create_directories(root);
    const fs::path wire = root / "wire.log", executable = ARENA_MOCK_UCI_PATH;
    const std::string type = GENERATE("string", "spin");
    arena::domain::TimeControl clock;
    clock.initial_ms = type == "spin" ? 4000000 : 180000;
    clock.increment_ms = 0;
    const auto result = arena::adapters::UciProcess::probe(executable, executable.parent_path(),
        {"--clock-option", "ArbitraryClockMs", "--clock-option-type", type, "--wire-log", wire.string()},
        {}, {{"ArbitraryClockMs", "0"}}, false, clock,
        {{"ArbitraryClockMs", arena::domain::ClockOptionField::initial_ms}});
    REQUIRE_FALSE(result.ok);
    const std::string transcript{std::istreambuf_iterator<char>(std::ifstream(wire).rdbuf()), {}};
    REQUIRE(transcript.find("setoption") == std::string::npos);
    REQUIRE(transcript.find("isready") == std::string::npos);
    REQUIRE(transcript.ends_with("quit\nowned-child-exit\n"));
    fs::remove_all(root);
}

TEST_CASE("clock probe bestmove timeout sends stop and joins the exact owned child") {
    namespace fs = std::filesystem;
    const fs::path root = fs::temp_directory_path() / ("arena-clock-timeout-" + arena::domain::uuid_v7());
    fs::create_directories(root);
    const fs::path wire = root / "wire.log", executable = ARENA_MOCK_UCI_PATH;
    arena::domain::TimeControl clock;
    clock.initial_ms = 180000;
    clock.increment_ms = 0;
    arena::adapters::UciProbeTimeouts timeouts;
    timeouts.first_bestmove = std::chrono::milliseconds(650);
    const auto result = arena::adapters::UciProcess::probe(executable, executable.parent_path(),
        {"--clock-option", "TestOriginalMs", "--expected-initial-ms", "180000", "--expected-increment-ms", "0",
            "--withhold-bestmove", "--wire-log", wire.string()}, timeouts, {{"TestOriginalMs", "0"}}, false, clock,
        {{"TestOriginalMs", arena::domain::ClockOptionField::initial_ms}});
    REQUIRE_FALSE(result.ok);
    REQUIRE(result.error.find("bestmove") != std::string::npos);
    const std::string transcript{std::istreambuf_iterator<char>(std::ifstream(wire).rdbuf()), {}};
    REQUIRE(transcript.find("go wtime 180000 btime 180000 winc 0 binc 0\nstop\n") != std::string::npos);
    REQUIRE(transcript.ends_with("quit\nowned-child-exit\n"));
    fs::remove_all(root);
}

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
    const fs::path temporary_parent = fs::canonical(fs::temp_directory_path());
    const fs::path cleanup_target = fs::weakly_canonical(root);
    REQUIRE(cleanup_target.is_absolute());
    REQUIRE(cleanup_target.parent_path() == temporary_parent);
    REQUIRE(cleanup_target.filename().string().starts_with("arena-timed-registration-"));
    REQUIRE(cleanup_target.filename() == root.filename());
    const auto original_status = fs::symlink_status(root);
    REQUIRE(fs::is_directory(original_status));
    REQUIRE_FALSE(fs::is_symlink(original_status));
    const auto cleanup_status = fs::symlink_status(cleanup_target);
    REQUIRE(fs::is_directory(cleanup_status));
    REQUIRE_FALSE(fs::is_symlink(cleanup_status));
    fs::remove_all(cleanup_target);
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
