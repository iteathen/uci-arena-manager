#include "arena/adapters/sqlite_store.hpp"
#include "arena/application/control_plane.hpp"
#include "arena/domain/uuid.hpp"

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>
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

}  // namespace

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
            control_plane, "match.plan", "match_plan", engines, 1);
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
        REQUIRE(wait_for_lifecycle(
                    control_plane, "match_plan", retained_id, "cancelled")
                    .at("data").at("lifecycle") == "cancelled");
    }
    fs::remove_all(root);
}
