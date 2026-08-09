#include "arena/adapters/sqlite_store.hpp"
#include "arena/adapters/uci_process.hpp"
#include "arena/application/control_plane.hpp"
#include "arena/domain/uuid.hpp"

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <thread>
#include <vector>

#ifndef ARENA_MOCK_UCI_PATH
#error ARENA_MOCK_UCI_PATH must name the mock UCI engine
#endif

namespace {

using nlohmann::json;

json command(
    const std::string& action,
    const std::string& kind,
    const json& id,
    const json& parameters = json::object(),
    const json& expected_revision = nullptr) {
    return {
        {"schema_version", 1},
        {"request_id", arena::domain::uuid_v7()},
        {"idempotency_key", arena::domain::uuid_v7()},
        {"target", {{"kind", kind}, {"id", id}}},
        {"action", action},
        {"expected_revision", expected_revision},
        {"dry_run", false},
        {"parameters", parameters},
    };
}

std::string register_engine(
    arena::application::ControlPlane& control_plane,
    const std::string& name,
    const std::vector<std::string>& arguments = {}) {
    const std::filesystem::path executable = ARENA_MOCK_UCI_PATH;
    return control_plane
        .submit(
            command(
                "engine.register",
                "engine",
                nullptr,
                {
                    {"display_name", name},
                    {"executable", executable.string()},
                    {"working_directory", executable.parent_path().string()},
                    {"arguments", arguments},
                }),
            "test")
        .at("resource")
        .at("id");
}

json resource_by_id(
    arena::application::ControlPlane& control_plane,
    const std::string& kind,
    const std::string& id) {
    for (const auto& resource : control_plane.list(kind, 10'000, 0)) {
        if (resource.at("id") == id) {
            return resource;
        }
    }
    return nullptr;
}

json wait_for_lifecycle(
    arena::application::ControlPlane& control_plane,
    const std::string& kind,
    const std::string& id,
    const std::string& lifecycle,
    const std::chrono::seconds timeout = std::chrono::seconds(10)) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        const json resource = resource_by_id(control_plane, kind, id);
        if (!resource.is_null() &&
            resource.at("data").value("lifecycle", "") == lifecycle) {
            return resource;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
    }
    return resource_by_id(control_plane, kind, id);
}

json create_competition(
    arena::application::ControlPlane& control_plane,
    const std::string& action,
    const std::string& kind,
    const std::vector<std::string>& engines,
    const unsigned games = 2,
    const json& overrides = json::object()) {
    json parameters{
        {"display_name", "Runtime integration"},
        {"format", kind == "match_plan" ? "head_to_head" : "round_robin"},
        {"variant", "standard"},
        {"engine_ids", engines},
        {"games_per_pair", games},
        {"rounds", 1},
        {"concurrency", 1},
        {"seed", 7},
        {"time_control", {{"initial_ms", 10'000}, {"increment_ms", 0}}},
    };
    parameters.update(overrides);
    return control_plane.submit(
        command(
            action,
            kind,
            nullptr,
            parameters),
        "test");
}

void seed_engine(
    const std::shared_ptr<arena::adapters::SqliteStore>& store,
    const std::string& id,
    const json& data) {
    const std::string timestamp = "2026-01-01T00:00:00.000Z";
    const std::string command_id = arena::domain::uuid_v7();
    store->create_resource(
        {
            id,
            "engine",
            "local",
            1,
            timestamp,
            timestamp,
            "{}",
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
            "engine.created",
            "engine",
            id,
            1,
            command_id,
            "{}",
        });
}

}  // namespace

TEST_CASE("legacy receipt engines launch tournaments with their packaged default model") {
    namespace fs = std::filesystem;
    const fs::path root = fs::temp_directory_path() /
        ("arena-tournament-default-model-" + arena::domain::uuid_v7());
    const fs::path engine_root = root / "engine-component";
    const fs::path model_root = engine_root / "models" / "default";
    const fs::path executable =
        engine_root / fs::path(ARENA_MOCK_UCI_PATH).filename();
    fs::create_directories(model_root);
    fs::copy_file(
        ARENA_MOCK_UCI_PATH,
        executable,
        fs::copy_options::overwrite_existing);
    {
        auto store = std::make_shared<arena::adapters::SqliteStore>(
            root / "arena.db");
        arena::application::ControlPlane control_plane(store);
        control_plane.initialize();
        const std::map<std::string, std::string> options{
            {"ModelRoot", fs::weakly_canonical(model_root).string()},
        };
        const auto probe = arena::adapters::UciProcess::probe(
            executable,
            engine_root,
            {"--require-model-root", model_root.string()},
            std::chrono::seconds(5),
            options);
        REQUIRE(probe.ok);
        const json engine_data{
            {"display_name", "Legacy receipt engine"},
            {"executable", executable.string()},
            {"working_directory", engine_root.string()},
            {"arguments", {"--require-model-root", model_root.string()}},
            {"option_overrides", json::object()},
            {"fingerprint", probe.fingerprint},
            {"onboarding_source", "install_receipt"},
            {"default_model",
             {{"model_id", "example_model_v1"},
              {"display_name", "Example Model"},
              {"input_adapter_id", "dense_planes_v1"},
              {"root", "models/default"}}},
        };
        const std::string first = arena::domain::uuid_v7();
        const std::string second = arena::domain::uuid_v7();
        seed_engine(store, first, engine_data);
        seed_engine(store, second, engine_data);
        const json created = create_competition(
            control_plane,
            "match.plan",
            "match_plan",
            {first, second},
            1,
            {{"adjudication", {{"max_plies", 2}}}});
        const std::string competition_id = created.at("resource").at("id");
        control_plane.submit(
            command(
                "match.start",
                "match_plan",
                competition_id,
                json::object(),
                created.at("resource").at("revision")),
            "test");
        const json completed = wait_for_lifecycle(
            control_plane,
            "match_plan",
            competition_id,
            "completed");
        REQUIRE(completed.at("data").at("lifecycle") == "completed");
        const auto games = control_plane.list("game", 10, 0);
        REQUIRE(games.size() == 1);
        REQUIRE(
            fs::path(
                games.at(0).at("data").at("white_engine").at("options")
                    .at("ModelRoot").get<std::string>()) ==
            fs::weakly_canonical(model_root));
        REQUIRE(
            fs::path(
                games.at(0).at("data").at("black_engine").at("options")
                    .at("ModelRoot").get<std::string>()) ==
            fs::weakly_canonical(model_root));
    }
    fs::remove_all(root);
}

TEST_CASE("competition options are validated against each selected engine") {
    namespace fs = std::filesystem;
    const fs::path root = fs::temp_directory_path() /
        ("arena-tournament-options-" + arena::domain::uuid_v7());
    {
        auto store = std::make_shared<arena::adapters::SqliteStore>(
            root / "arena.db");
        arena::application::ControlPlane control_plane(store);
        control_plane.initialize();
        const std::vector<std::string> engines{
            register_engine(control_plane, "Option Alpha"),
            register_engine(control_plane, "Option Beta"),
        };

        try {
            (void)create_competition(
                control_plane,
                "match.plan",
                "match_plan",
                engines,
                1,
                {{"engine_option_overrides",
                  {{engines.at(0), {{"Threads", "99"}}}}}});
            FAIL("out-of-range engine option was accepted");
        } catch (const arena::application::CommandFailure& error) {
            REQUIRE(error.code() == "invalid_engine_options");
        }

        try {
            (void)create_competition(
                control_plane,
                "match.plan",
                "match_plan",
                engines,
                1,
                {{"engine_option_overrides",
                  {{engines.at(0), {{"UnknownOption", "value"}}}}}});
            FAIL("unknown engine option was accepted");
        } catch (const arena::application::CommandFailure& error) {
            REQUIRE(error.code() == "invalid_engine_options");
        }

        const json valid = create_competition(
            control_plane,
            "match.plan",
            "match_plan",
            engines,
            1,
            {{"engine_option_overrides",
              {{engines.at(0), {{"Threads", "4"}, {"Style", "Solid"}}}}}});
        REQUIRE(
            valid.at("resource")
                    .at("data")
                    .at("engine_option_overrides")
                    .at(engines.at(0))
                    .at("Threads") == "4");
    }
    fs::remove_all(root);
}

TEST_CASE("changed engine identity fails before tournament game admission") {
    namespace fs = std::filesystem;
    const fs::path root = fs::temp_directory_path() /
        ("arena-tournament-identity-" + arena::domain::uuid_v7());
    fs::create_directories(root);
    const fs::path executable =
        root / fs::path(ARENA_MOCK_UCI_PATH).filename();
    fs::copy_file(
        ARENA_MOCK_UCI_PATH,
        executable,
        fs::copy_options::overwrite_existing);
    {
        auto store = std::make_shared<arena::adapters::SqliteStore>(
            root / "arena.db");
        arena::application::ControlPlane control_plane(store);
        control_plane.initialize();
        const auto register_copy = [&](const std::string& name) {
            return control_plane
                .submit(
                    command(
                        "engine.register",
                        "engine",
                        nullptr,
                        {
                            {"display_name", name},
                            {"executable", executable.string()},
                            {"working_directory", root.string()},
                            {"arguments", json::array()},
                        }),
                    "test")
                .at("resource")
                .at("id")
                .get<std::string>();
        };
        const std::vector<std::string> engines{
            register_copy("Identity Alpha"),
            register_copy("Identity Beta"),
        };
        const json created = create_competition(
            control_plane,
            "match.plan",
            "match_plan",
            engines,
            1);
        std::ofstream(executable, std::ios::binary | std::ios::app).put('\0');

        const std::string id = created.at("resource").at("id");
        control_plane.submit(
            command(
                "match.start",
                "match_plan",
                id,
                json::object(),
                created.at("resource").at("revision")),
            "test");
        const json failed = wait_for_lifecycle(
            control_plane, "match_plan", id, "failed");
        REQUIRE(failed.at("data").at("lifecycle") == "failed");
        INFO(failed.at("data").at("failure_detail").get<std::string>());
        REQUIRE(
            failed.at("data")
                .at("failure_detail")
                .get<std::string>()
                .find("registered engine identity changed") !=
            std::string::npos);
        REQUIRE(control_plane.list("game", 10, 0).empty());
    }
    fs::remove_all(root);
}

TEST_CASE("qualified runtime identity is revalidated before game admission") {
    namespace fs = std::filesystem;
    const fs::path root = fs::temp_directory_path() /
        ("arena-tournament-runtime-" + arena::domain::uuid_v7());
    fs::create_directories(root);
    {
        auto store = std::make_shared<arena::adapters::SqliteStore>(
            root / "arena.db");
        arena::application::ControlPlane control_plane(store);
        control_plane.initialize();
        const std::vector<std::string> engines{
            register_engine(control_plane, "Runtime Alpha"),
            register_engine(control_plane, "Runtime Beta"),
        };
        const auto existing = store->get_resource("engine", engines.at(0));
        REQUIRE(existing.has_value());
        arena::ports::ResourceRecord updated = *existing;
        json engine_data = json::parse(updated.data_json);
        engine_data["launch_profile"] = {
            {"schema", "arena_uci_engine_launch_profile_v1"},
            {"state", "qualified_timing"},
            {"timing_profile", {{"available", true}}},
            {"expected_runtime",
             {
                 {"timing_contract_version", "persistent_mcgs_relative_clock_temporal_v4"},
                 {"model_id", "example_model_v1"},
                 {"model_fingerprint", "8104116015256418347"},
                 {"adapter_id", "dense_planes_v1"},
                 {"provider_requested", "cpu"},
                 {"provider_effective", "cpu"},
                 {"provider_numeric_fingerprint", "5805769137532863231"},
                 {"hardware_driver_fingerprint", "15113055170282736144"},
                 {"onnx_runtime_version", "1.24.4"},
                 {"search_core", "mcgs"},
             }},
        };
        const std::string now = arena::domain::utc_now();
        const std::string command_id = arena::domain::uuid_v7();
        updated.revision += 1;
        updated.updated_at = now;
        updated.data_json = engine_data.dump();
        store->update_resource(
            updated,
            existing->revision,
            {
                command_id,
                arena::domain::uuid_v7(),
                arena::domain::uuid_v7(),
                "test.engine.runtime_drift",
                "completed",
                "{}",
                now,
            },
            {
                0,
                arena::domain::uuid_v7(),
                now,
                "state",
                "engine.runtime_drifted",
                "engine",
                updated.id,
                updated.revision,
                command_id,
                "{}",
            });

        const json created = create_competition(
            control_plane, "match.plan", "match_plan", engines, 1);
        const std::string id = created.at("resource").at("id");
        control_plane.submit(
            command(
                "match.start",
                "match_plan",
                id,
                json::object(),
                created.at("resource").at("revision")),
            "test");
        const json failed = wait_for_lifecycle(
            control_plane, "match_plan", id, "failed");
        REQUIRE(failed.at("data").at("lifecycle") == "failed");
        INFO(failed.at("data").at("failure_detail").get<std::string>());
        REQUIRE(
            failed.at("data").at("failure_detail").get<std::string>().find(
                "hardware_driver_fingerprint") != std::string::npos);
        REQUIRE(control_plane.list("game", 10, 0).empty());
    }
    fs::remove_all(root);
}

TEST_CASE("tournament execution commits legal plies and truthful progress") {
    namespace fs = std::filesystem;
    const fs::path root = fs::temp_directory_path() /
        ("arena-tournament-runtime-" + arena::domain::uuid_v7());
    {
        auto store = std::make_shared<arena::adapters::SqliteStore>(root / "arena.db");
        arena::application::ControlPlane control_plane(store);
        control_plane.initialize();

    const std::vector<std::string> engines{
        register_engine(control_plane, "Mock Alpha"),
        register_engine(control_plane, "Mock Beta"),
    };
    const json created = create_competition(
        control_plane, "tournament.plan", "tournament", engines);
    const std::string id = created.at("resource").at("id");
    control_plane.submit(
        command(
            "tournament.start",
            "tournament",
            id,
            json::object(),
            created.at("resource").at("revision")),
        "test");

    const json completed = wait_for_lifecycle(
        control_plane, "tournament", id, "completed");
    REQUIRE(completed.at("data").at("lifecycle") == "completed");
    const json progress = completed.at("data").at("progress");
    REQUIRE(progress.at("total_games") == 2);
    REQUIRE(progress.at("completed_games") == 2);
    REQUIRE(progress.at("active_games") == 0);
    REQUIRE(progress.at("queued_games") == 0);
    REQUIRE(progress.at("cancelled_games") == 0);
    REQUIRE(progress.at("failed_games") == 0);
    REQUIRE(progress.at("unplayed_games") == 0);
    REQUIRE(progress.at("percentage") == 100.0);

    std::size_t games = 0;
    for (const auto& game : control_plane.list("game", 100, 0)) {
        if (game.at("data").value("competition_id", "") != id) {
            continue;
        }
        ++games;
        const json data = game.at("data");
        REQUIRE(data.at("lifecycle") == "completed");
        REQUIRE(data.at("result") == "1-0");
        REQUIRE(data.at("termination") == "checkmate");
        REQUIRE(data.at("uci_moves").size() == 7);
        REQUIRE(data.at("san_moves").size() == 7);
        REQUIRE(data.at("per_ply_fen").size() == 7);
        REQUIRE(data.at("last_committed_ply") == 7);
    }
        REQUIRE(games == 2);
    }
    fs::remove_all(root);
}

TEST_CASE("startup reconciles legacy terminal competition progress once") {
    namespace fs = std::filesystem;
    const fs::path root = fs::temp_directory_path() /
        ("arena-tournament-progress-migration-" + arena::domain::uuid_v7());
    auto store = std::make_shared<arena::adapters::SqliteStore>(root / "arena.db");
    store->initialize();
    const std::string id = arena::domain::uuid_v7();
    const std::string timestamp = "2026-01-01T00:00:00.000Z";
    const std::string command_id = arena::domain::uuid_v7();
    store->create_resource(
        {
            id,
            "tournament",
            "local",
            1,
            timestamp,
            timestamp,
            "{}",
            json({
                {"lifecycle", "cancelled"},
                {"planned_total_games", 2},
                {"pairings", json::array({json::object(), json::object()})},
                {"progress",
                 {{"total_games", 2},
                  {"queued_games", 2},
                  {"active_games", 0},
                  {"completed_games", 0},
                  {"percentage", 0.0},
                  {"current_round", nullptr},
                  {"updated_at", timestamp}}},
            }).dump(),
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
            "competition.seeded",
            "tournament",
            id,
            1,
            command_id,
            "{}",
        });

    {
        arena::application::ControlPlane control_plane(store);
        control_plane.initialize();
        const json reconciled = resource_by_id(control_plane, "tournament", id);
        const json progress = reconciled.at("data").at("progress");
        REQUIRE(progress.at("queued_games") == 0);
        REQUIRE(progress.at("active_games") == 0);
        REQUIRE(progress.at("unplayed_games") == 2);
        REQUIRE(progress.at("percentage") == 100.0);
        const std::uint64_t reconciled_revision =
            reconciled.at("revision").get<std::uint64_t>();
        control_plane.initialize();
        REQUIRE(
            resource_by_id(control_plane, "tournament", id)
                .at("revision").get<std::uint64_t>() == reconciled_revision);
    }
    store.reset();
    fs::remove_all(root);
}

TEST_CASE("restart pauses an active match and resume continues committed state") {
    namespace fs = std::filesystem;
    const fs::path root = fs::temp_directory_path() /
        ("arena-tournament-recovery-" + arena::domain::uuid_v7());
    auto store = std::make_shared<arena::adapters::SqliteStore>(root / "arena.db");
    std::string id;
    {
        arena::application::ControlPlane control_plane(store);
        control_plane.initialize();
        const std::vector<std::string> engines{
            register_engine(control_plane, "Slow Alpha", {"--delay-ms", "120"}),
            register_engine(control_plane, "Slow Beta", {"--delay-ms", "120"}),
        };
        const json created = create_competition(
            control_plane, "match.plan", "match_plan", engines, 1);
        id = created.at("resource").at("id");
        control_plane.submit(
            command(
                "match.start",
                "match_plan",
                id,
                json::object(),
                created.at("resource").at("revision")),
            "test");

        const auto deadline = std::chrono::steady_clock::now() +
            std::chrono::seconds(5);
        bool committed = false;
        while (std::chrono::steady_clock::now() < deadline && !committed) {
            for (const auto& game : control_plane.list("game", 100, 0)) {
                committed = game.at("data").value("competition_id", "") == id &&
                    !game.at("data").value("uci_moves", json::array()).empty();
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(25));
        }
        REQUIRE(committed);
    }

    {
        arena::application::ControlPlane control_plane(store);
        control_plane.initialize();
        const json paused = resource_by_id(control_plane, "match_plan", id);
        REQUIRE(paused.at("data").at("lifecycle") == "paused");
        control_plane.submit(
            command(
                "match.resume",
                "match_plan",
                id,
                json::object(),
                paused.at("revision")),
            "test");
        const json completed = wait_for_lifecycle(
            control_plane, "match_plan", id, "completed");
        REQUIRE(completed.at("data").at("lifecycle") == "completed");
        const json games = control_plane.list("game", 100, 0);
        REQUIRE(games.size() == 1);
        REQUIRE(games.at(0).at("data").at("uci_moves").size() == 7);
        REQUIRE(games.at(0).at("data").at("termination") == "checkmate");
    }
    store.reset();
    fs::remove_all(root);
}

TEST_CASE("Swiss execution appends deterministic non-repeating rounds") {
    namespace fs = std::filesystem;
    const fs::path root = fs::temp_directory_path() /
        ("arena-swiss-runtime-" + arena::domain::uuid_v7());
    {
        auto store = std::make_shared<arena::adapters::SqliteStore>(root / "arena.db");
        arena::application::ControlPlane control_plane(store);
        control_plane.initialize();
        std::vector<std::string> engines;
        for (const auto* name : {"Alpha", "Beta", "Gamma", "Delta"}) {
            engines.push_back(register_engine(control_plane, name));
        }
        const json created = create_competition(
            control_plane,
            "tournament.plan",
            "tournament",
            engines,
            1,
            {{"format", "swiss"}, {"rounds", 3}, {"seed", 13}});
        const std::string id = created.at("resource").at("id");
        control_plane.submit(
            command(
                "tournament.start", "tournament", id, json::object(),
                created.at("resource").at("revision")),
            "test");
        const json completed = wait_for_lifecycle(
            control_plane, "tournament", id, "completed", std::chrono::seconds(20));
        REQUIRE(completed.at("data").at("lifecycle") == "completed");
        REQUIRE(completed.at("data").at("pairings").size() == 6);
        REQUIRE(completed.at("data").at("progress").at("completed_games") == 6);
        std::set<std::set<std::string>> opponents;
        std::set<unsigned> rounds;
        for (const auto& game : control_plane.list("game", 100, 0)) {
            if (game.at("data").value("competition_id", "") != id) continue;
            opponents.insert({
                game.at("data").at("white_engine").at("id").get<std::string>(),
                game.at("data").at("black_engine").at("id").get<std::string>(),
            });
            rounds.insert(game.at("data").at("round").get<unsigned>());
        }
        REQUIRE(opponents.size() == 6);
        REQUIRE(rounds == std::set<unsigned>{1, 2, 3});
    }
    fs::remove_all(root);
}

TEST_CASE("odd-player Swiss rotates byes and persists final standings") {
    namespace fs = std::filesystem;
    const fs::path root = fs::temp_directory_path() /
        ("arena-swiss-byes-" + arena::domain::uuid_v7());
    {
        auto store = std::make_shared<arena::adapters::SqliteStore>(root / "arena.db");
        arena::application::ControlPlane control_plane(store);
        control_plane.initialize();
        std::vector<std::string> engines;
        for (const auto* name : {"Alpha", "Beta", "Gamma"}) {
            engines.push_back(register_engine(control_plane, name));
        }
        const json created = create_competition(
            control_plane,
            "tournament.plan",
            "tournament",
            engines,
            1,
            {{"format", "swiss"}, {"rounds", 3}, {"seed", 19}});
        const std::string id = created.at("resource").at("id");
        control_plane.submit(
            command(
                "tournament.start", "tournament", id, json::object(),
                created.at("resource").at("revision")),
            "test");
        const json completed = wait_for_lifecycle(
            control_plane, "tournament", id, "completed", std::chrono::seconds(20));
        REQUIRE(completed.at("data").at("lifecycle") == "completed");
        REQUIRE(completed.at("data").at("progress").at("total_games") == 3);
        const json byes = completed.at("data").at("swiss_byes");
        REQUIRE(byes.size() == 3);
        std::set<std::string> bye_engines;
        for (const auto& bye : byes) {
            bye_engines.insert(bye.at("engine_id").get<std::string>());
        }
        REQUIRE(bye_engines.size() == 3);
        double score = 0.0;
        for (const auto& standing : completed.at("data").at("swiss_standings")) {
            score += standing.at("score").get<double>();
        }
        REQUIRE(score == 6.0);
    }
    fs::remove_all(root);
}

TEST_CASE("SPRT persists a terminal color-paired likelihood verdict") {
    namespace fs = std::filesystem;
    const fs::path root = fs::temp_directory_path() /
        ("arena-sprt-runtime-" + arena::domain::uuid_v7());
    {
        auto store = std::make_shared<arena::adapters::SqliteStore>(root / "arena.db");
        arena::application::ControlPlane control_plane(store);
        control_plane.initialize();
        const std::vector<std::string> engines{
            register_engine(control_plane, "Reference"),
            register_engine(
                control_plane, "Weaker", {"--illegal-after-ply", "1"}),
        };
        const json created = create_competition(
            control_plane,
            "tournament.plan",
            "tournament",
            engines,
            6,
            {
                {"format", "sprt"},
                {"concurrency", 2},
                {"sprt", {{"elo0", 0}, {"elo1", 400},
                           {"alpha", 0.25}, {"beta", 0.25}}},
            });
        const std::string id = created.at("resource").at("id");
        control_plane.submit(
            command(
                "tournament.start", "tournament", id, json::object(),
                created.at("resource").at("revision")),
            "test");
        const json completed = wait_for_lifecycle(
            control_plane, "tournament", id, "completed");
        const json sprt = completed.at("data").at("sprt");
        REQUIRE(sprt.at("verdict") == "accept_h1");
        REQUIRE(sprt.at("games") == 2);
        REQUIRE(sprt.at("score") == 2.0);
        REQUIRE(completed.at("data").at("progress").at("total_games") == 2);
        REQUIRE(control_plane.list("game", 100, 0).size() == 2);
    }
    fs::remove_all(root);
}

TEST_CASE("Chess960 execution persists its FEN and enables the UCI variant") {
    namespace fs = std::filesystem;
    const fs::path root = fs::temp_directory_path() /
        ("arena-chess960-runtime-" + arena::domain::uuid_v7());
    {
        auto store = std::make_shared<arena::adapters::SqliteStore>(root / "arena.db");
        arena::application::ControlPlane control_plane(store);
        control_plane.initialize();
        const std::vector<std::string> engines{
            register_engine(control_plane, "960 Alpha", {"--require-chess960"}),
            register_engine(control_plane, "960 Beta", {"--require-chess960"}),
        };
        const json created = create_competition(
            control_plane,
            "match.plan",
            "match_plan",
            engines,
            1,
            {{"variant", "chess960"}, {"seed", 518}});
        const std::string id = created.at("resource").at("id");
        control_plane.submit(
            command(
                "match.start", "match_plan", id, json::object(),
                created.at("resource").at("revision")),
            "test");
        REQUIRE(wait_for_lifecycle(control_plane, "match_plan", id, "completed")
                    .at("data").at("lifecycle") == "completed");
        const json game = control_plane.list("game", 10, 0).at(0).at("data");
        REQUIRE(game.at("variant") == "chess960");
        REQUIRE(game.at("starting_fen") ==
            "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w HAha - 0 1");
        REQUIRE(game.at("termination") == "checkmate");
    }
    fs::remove_all(root);
}

TEST_CASE("opening suite moves and bounded evaluation adjudication are applied") {
    namespace fs = std::filesystem;
    const fs::path root = fs::temp_directory_path() /
        ("arena-opening-runtime-" + arena::domain::uuid_v7());
    fs::create_directories(root);
    const fs::path suite = root / "openings.json";
    std::ofstream(suite) << json{
        {"schema", "arena_opening_suite_v1"},
        {"positions", json::array({{
            {"variant", "standard"},
            {"starting_fen",
             "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1"},
            {"uci_moves", {"e2e4", "e7e5"}},
        }})},
    };
    {
        auto store = std::make_shared<arena::adapters::SqliteStore>(root / "arena.db");
        arena::application::ControlPlane control_plane(store);
        control_plane.initialize();
        const std::vector<std::string> engines{
            register_engine(
                control_plane, "Timed Alpha", {"--require-movestogo", "40"}),
            register_engine(
                control_plane, "Timed Beta", {"--require-movestogo", "40"}),
        };
        const json created = create_competition(
            control_plane,
            "match.plan",
            "match_plan",
            engines,
            1,
            {
                {"opening_suite", suite.string()},
                {"time_control",
                 {{"initial_ms", 10'000}, {"increment_ms", 0},
                  {"moves_to_go", 40}}},
                {"adjudication",
                 {{"max_plies", 64}, {"draw_cp", 20},
                  {"draw_consecutive_plies", 2}, {"draw_min_plies", 4}}},
            });
        const std::string id = created.at("resource").at("id");
        control_plane.submit(
            command(
                "match.start", "match_plan", id, json::object(),
                created.at("resource").at("revision")),
            "test");
        REQUIRE(wait_for_lifecycle(control_plane, "match_plan", id, "completed")
                    .at("data").at("lifecycle") == "completed");
        const json game = control_plane.list("game", 10, 0).at(0).at("data");
        REQUIRE(game.at("opening_plies") == 2);
        REQUIRE(game.at("uci_moves").size() == 4);
        REQUIRE(game.at("evaluations_cp").size() == 4);
        REQUIRE(game.at("principal_variations").size() == 4);
        REQUIRE(game.at("result") == "1/2-1/2");
        REQUIRE(game.at("termination") == "evaluation_draw_adjudication");
    }
    fs::remove_all(root);
}

TEST_CASE("only never-started competition plans can be removed") {
    namespace fs = std::filesystem;
    const fs::path root = fs::temp_directory_path() /
        ("arena-competition-removal-" + arena::domain::uuid_v7());
    {
        auto store = std::make_shared<arena::adapters::SqliteStore>(root / "arena.db");
        arena::application::ControlPlane control_plane(store);
        control_plane.initialize();
        const std::vector<std::string> engines{
            register_engine(control_plane, "Removal Alpha", {"--delay-ms", "100"}),
            register_engine(control_plane, "Removal Beta", {"--delay-ms", "100"}),
        };

        const json removable = create_competition(
            control_plane, "match.plan", "match_plan", engines, 1);
        const std::string removable_id = removable.at("resource").at("id");
        try {
            static_cast<void>(control_plane.submit(
                command(
                    "resource.remove",
                    "match_plan",
                    removable_id,
                    json::object(),
                    removable.at("resource").at("revision").get<std::uint64_t>() + 1),
                "test"));
            FAIL("stale plan removal unexpectedly succeeded");
        } catch (const arena::application::CommandFailure& error) {
            REQUIRE(error.code() == "resource_revision_conflict");
        }
        static_cast<void>(control_plane.submit(
            command(
                "resource.remove",
                "match_plan",
                removable_id,
                json::object(),
                removable.at("resource").at("revision")),
            "test"));
        REQUIRE(resource_by_id(
                    control_plane, "match_plan", removable_id).is_null());

        const json retained = create_competition(
            control_plane, "match.plan", "match_plan", engines, 2);
        const std::string retained_id = retained.at("resource").at("id");
        const json running = control_plane.submit(
            command(
                "match.start",
                "match_plan",
                retained_id,
                json::object(),
                retained.at("resource").at("revision")),
            "test");
        REQUIRE_THROWS_AS(
            control_plane.submit(
                command(
                    "resource.remove",
                    "match_plan",
                    retained_id,
                    json::object(),
                    running.at("resource").at("revision")),
                "test"),
            arena::application::CommandFailure);
        bool ply_committed = false;
        const auto active_deadline = std::chrono::steady_clock::now() +
            std::chrono::seconds(5);
        while (std::chrono::steady_clock::now() < active_deadline &&
               !ply_committed) {
            for (const auto& game : control_plane.list("game", 100, 0)) {
                ply_committed =
                    game.at("data").value("competition_id", "") == retained_id &&
                    !game.at("data").value("uci_moves", json::array()).empty();
                if (ply_committed) {
                    break;
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(25));
        }
        REQUIRE(ply_committed);
        bool cancel_submitted = false;
        for (int attempt = 0; attempt < 20 && !cancel_submitted; ++attempt) {
            const json current = resource_by_id(
                control_plane, "match_plan", retained_id);
            try {
                static_cast<void>(control_plane.submit(
                    command(
                        "match.cancel",
                        "match_plan",
                        retained_id,
                        json::object(),
                        current.at("revision")),
                    "test"));
                cancel_submitted = true;
            } catch (const arena::application::CommandFailure& error) {
                if (error.code() != "resource_revision_conflict") {
                    throw;
                }
            }
        }
        REQUIRE(cancel_submitted);
        json cancelled;
        const auto deadline = std::chrono::steady_clock::now() +
            std::chrono::seconds(10);
        while (std::chrono::steady_clock::now() < deadline) {
            cancelled = resource_by_id(
                control_plane, "match_plan", retained_id);
            const json progress = cancelled.at("data").at("progress");
            if (cancelled.at("data").at("lifecycle") == "cancelled" &&
                progress.at("active_games") == 0 &&
                progress.at("percentage") == 100.0) {
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(25));
        }
        const json progress = cancelled.at("data").at("progress");
        REQUIRE(cancelled.at("data").at("lifecycle") == "cancelled");
        REQUIRE(progress.at("active_games") == 0);
        REQUIRE(progress.at("queued_games") == 0);
        REQUIRE(progress.at("percentage") == 100.0);
        REQUIRE(
            progress.at("total_games").get<std::size_t>() ==
            progress.at("completed_games").get<std::size_t>() +
                progress.at("cancelled_games").get<std::size_t>() +
                progress.at("failed_games").get<std::size_t>() +
                progress.at("unplayed_games").get<std::size_t>());
        REQUIRE(
            progress.at("cancelled_games").get<std::size_t>() +
                progress.at("unplayed_games").get<std::size_t>() > 0);
    }
    fs::remove_all(root);
}
