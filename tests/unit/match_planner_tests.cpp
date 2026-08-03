#include "arena/domain/match_planner.hpp"

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <limits>
#include <string>

namespace {

arena::domain::MatchPlan base_plan() {
    arena::domain::MatchPlan plan;
    plan.id = "018f218e-7be1-7a56-9daa-3f003641c801";
    plan.display_name = "Registered engine round robin";
    plan.format = arena::domain::TournamentFormat::round_robin;
    plan.engine_ids = {
        "018f218e-7be1-7a56-9daa-3f003641c811",
        "018f218e-7be1-7a56-9daa-3f003641c812",
        "018f218e-7be1-7a56-9daa-3f003641c813",
    };
    plan.games_per_pair = 2;
    plan.seed = 42;
    return plan;
}

}  // namespace

TEST_CASE("round robin produces color-paired games for every engine pair") {
    auto plan = base_plan();
    const auto scheduled =
        arena::domain::MatchPlanner::normalize_and_schedule(plan);
    REQUIRE(scheduled.pairings.size() == 6);
    for (std::size_t index = 0;
         index < scheduled.pairings.size();
         index += 2) {
        REQUIRE(
            scheduled.pairings[index].white_engine_id ==
            scheduled.pairings[index + 1].black_engine_id);
        REQUIRE(
            scheduled.pairings[index].black_engine_id ==
            scheduled.pairings[index + 1].white_engine_id);
    }
}

TEST_CASE("head to head accepts exactly two engines") {
    auto plan = base_plan();
    plan.format = arena::domain::TournamentFormat::head_to_head;
    REQUIRE_THROWS(
        arena::domain::MatchPlanner::normalize_and_schedule(plan));
    plan.engine_ids.pop_back();
    const auto scheduled =
        arena::domain::MatchPlanner::normalize_and_schedule(plan);
    REQUIRE(scheduled.pairings.size() == 2);
}

TEST_CASE("Swiss first round is deterministic for a seed") {
    auto plan = base_plan();
    plan.engine_ids.push_back(
        "018f218e-7be1-7a56-9daa-3f003641c814");
    plan.format = arena::domain::TournamentFormat::swiss;
    const auto first =
        arena::domain::MatchPlanner::normalize_and_schedule(plan);
    const auto second =
        arena::domain::MatchPlanner::normalize_and_schedule(plan);
    REQUIRE(first.pairings.size() == 2);
    REQUIRE(
        first.pairings.front().white_engine_id ==
        second.pairings.front().white_engine_id);
    REQUIRE(
        first.pairings.front().black_engine_id ==
        second.pairings.front().black_engine_id);
}

TEST_CASE("color-paired opening indices remain aligned") {
    auto plan = base_plan();
    plan.engine_ids.pop_back();
    plan.format = arena::domain::TournamentFormat::head_to_head;
    plan.games_per_pair = 4;
    const auto scheduled =
        arena::domain::MatchPlanner::normalize_and_schedule(plan);
    REQUIRE(scheduled.pairings.at(0).opening_index == 42);
    REQUIRE(scheduled.pairings.at(1).opening_index == 42);
    REQUIRE(scheduled.pairings.at(2).opening_index == 43);
    REQUIRE(scheduled.pairings.at(3).opening_index == 43);
}

TEST_CASE("SPRT rejects unpaired games and invalid decision bounds") {
    auto plan = base_plan();
    plan.engine_ids.pop_back();
    plan.format = arena::domain::TournamentFormat::sprt;
    plan.games_per_pair = 3;
    REQUIRE_THROWS(
        arena::domain::MatchPlanner::normalize_and_schedule(plan));

    plan.games_per_pair = 4;
    plan.sprt.elo0 = 10;
    plan.sprt.elo1 = 10;
    REQUIRE_THROWS(
        arena::domain::MatchPlanner::normalize_and_schedule(plan));

    plan.sprt.elo1 = 20;
    plan.sprt.alpha = 0.5;
    REQUIRE_THROWS(
        arena::domain::MatchPlanner::normalize_and_schedule(plan));
}

TEST_CASE("SPRT rejects nonfinite values") {
    auto plan = base_plan();
    plan.engine_ids.pop_back();
    plan.format = arena::domain::TournamentFormat::sprt;
    plan.games_per_pair = 4;
    plan.sprt.elo0 = std::numeric_limits<double>::quiet_NaN();
    REQUIRE_THROWS_WITH(
        arena::domain::MatchPlanner::normalize_and_schedule(plan),
        "SPRT values must be finite");

    plan.sprt.elo0 = 0.0;
    plan.sprt.alpha = std::numeric_limits<double>::infinity();
    REQUIRE_THROWS_WITH(
        arena::domain::MatchPlanner::normalize_and_schedule(plan),
        "SPRT values must be finite");
}

TEST_CASE("time control and adjudication policy enforce contract bounds") {
    auto plan = base_plan();
    plan.time_control.moves_to_go = 0;
    REQUIRE_THROWS_WITH(
        arena::domain::MatchPlanner::normalize_and_schedule(plan),
        "time control moves_to_go must be positive");

    plan.time_control.moves_to_go = 40;
    plan.adjudication.max_plies = 2049;
    REQUIRE_THROWS_WITH(
        arena::domain::MatchPlanner::normalize_and_schedule(plan),
        "adjudication max_plies must be between 2 and 2048");

    plan.adjudication.max_plies = 512;
    plan.adjudication.draw_cp = 1001;
    REQUIRE_THROWS_WITH(
        arena::domain::MatchPlanner::normalize_and_schedule(plan),
        "adjudication draw_cp must be between 0 and 1000");

    plan.adjudication.draw_cp.reset();
    plan.adjudication.win_consecutive_plies = 0;
    REQUIRE_THROWS_WITH(
        arena::domain::MatchPlanner::normalize_and_schedule(plan),
        "adjudication consecutive-ply bounds are invalid");
}

TEST_CASE("schedule cardinality is recovery bounded before allocation") {
    auto plan = base_plan();
    plan.engine_ids.pop_back();
    plan.format = arena::domain::TournamentFormat::head_to_head;
    plan.games_per_pair = 10'000;
    plan.concurrency = 256;
    const auto accepted =
        arena::domain::MatchPlanner::normalize_and_schedule(plan);
    REQUIRE(accepted.pairings.size() == 10'000);

    plan.games_per_pair = 10'001;
    REQUIRE_THROWS_WITH(
        arena::domain::MatchPlanner::normalize_and_schedule(plan),
        "competition schedule must contain 1..10000 games");

    plan = base_plan();
    plan.games_per_pair = 1;
    for (int index = 0; index < 150; ++index) {
        plan.engine_ids.push_back("engine-" + std::to_string(index));
    }
    REQUIRE_THROWS_WITH(
        arena::domain::MatchPlanner::normalize_and_schedule(plan),
        "competition schedule must contain 1..10000 games");
}

TEST_CASE("concurrency is bounded by capacity and immediate admission") {
    auto plan = base_plan();
    plan.engine_ids.pop_back();
    plan.format = arena::domain::TournamentFormat::head_to_head;
    plan.games_per_pair = 2;

    plan.concurrency = 257;
    REQUIRE_THROWS_WITH(
        arena::domain::MatchPlanner::normalize_and_schedule(plan),
        "concurrency must be between 1 and 256");

    plan.concurrency = 3;
    REQUIRE_THROWS_WITH(
        arena::domain::MatchPlanner::normalize_and_schedule(plan),
        "concurrency cannot exceed the currently admittable game count");

    plan = base_plan();
    plan.engine_ids.push_back(
        "018f218e-7be1-7a56-9daa-3f003641c814");
    plan.format = arena::domain::TournamentFormat::swiss;
    plan.rounds = 100;
    plan.concurrency = 3;
    REQUIRE_THROWS_WITH(
        arena::domain::MatchPlanner::normalize_and_schedule(plan),
        "concurrency cannot exceed the currently admittable game count");
}

TEST_CASE("engine IDs are bounded distinct UTF-8 scalar text") {
    auto plan = base_plan();
    plan.engine_ids[1] = plan.engine_ids[0];
    REQUIRE_THROWS_WITH(
        arena::domain::MatchPlanner::normalize_and_schedule(plan),
        "engine IDs in a local match plan must be distinct");

    plan = base_plan();
    plan.engine_ids[0] = std::string("\xc0\xaf", 2);
    REQUIRE_THROWS_WITH(
        arena::domain::MatchPlanner::normalize_and_schedule(plan),
        "engine IDs must be bounded UTF-8 scalar text");
}

TEST_CASE("engine option overrides have bounded owners and text") {
    auto plan = base_plan();
    plan.engine_option_overrides["outside-engine"]["Threads"] = "4";
    REQUIRE_THROWS_WITH(
        arena::domain::MatchPlanner::normalize_and_schedule(plan),
        "engine option overrides name an engine outside the plan");

    plan.engine_option_overrides.clear();
    plan.engine_option_overrides[plan.engine_ids.front()]["Bad\tOption"] = "4";
    REQUIRE_THROWS_WITH(
        arena::domain::MatchPlanner::normalize_and_schedule(plan),
        "engine option override is invalid or too large");

    plan.engine_option_overrides.clear();
    for (int index = 0; index < 129; ++index) {
        plan.engine_option_overrides[plan.engine_ids.front()][
            "Option" + std::to_string(index)] = "value";
    }
    REQUIRE_THROWS_WITH(
        arena::domain::MatchPlanner::normalize_and_schedule(plan),
        "engine option override count exceeds 128");
}

TEST_CASE("aggregate engine option metadata is bounded") {
    auto plan = base_plan();
    plan.format = arena::domain::TournamentFormat::swiss;
    plan.engine_ids.clear();
    for (int index = 0; index < 258; ++index) {
        plan.engine_ids.push_back("engine-" + std::to_string(index));
    }
    for (int engine = 0; engine < 129; ++engine) {
        auto& overrides = plan.engine_option_overrides[
            plan.engine_ids[static_cast<std::size_t>(engine)]];
        for (int option = 0; option < 128; ++option) {
            overrides["Option" + std::to_string(option)] = "value";
        }
    }
    REQUIRE_THROWS_WITH(
        arena::domain::MatchPlanner::normalize_and_schedule(plan),
        "total engine option override count exceeds 16384");
}

TEST_CASE("clock accumulation remains representable through max plies") {
    auto plan = base_plan();
    plan.time_control.initial_ms =
        static_cast<std::uint64_t>(
            (std::numeric_limits<std::int64_t>::max)()) -
        2'000;
    plan.time_control.increment_ms = 1;
    REQUIRE_THROWS_WITH(
        arena::domain::MatchPlanner::normalize_and_schedule(plan),
        "time control can overflow before max_plies");

    plan = base_plan();
    plan.time_control.initial_ms = 60'000;
    plan.time_control.increment_ms =
        static_cast<std::uint64_t>(
            (std::numeric_limits<std::int64_t>::max)());
    REQUIRE_THROWS_WITH(
        arena::domain::MatchPlanner::normalize_and_schedule(plan),
        "time control exceeds signed chrono millisecond capacity");
}

TEST_CASE("display names are normalized then bounded") {
    auto plan = base_plan();
    plan.display_name.clear();
    plan.engine_ids.pop_back();
    plan.format = arena::domain::TournamentFormat::head_to_head;
    const auto normalized =
        arena::domain::MatchPlanner::normalize_and_schedule(plan);
    REQUIRE(normalized.display_name == "Local head_to_head");

    plan = base_plan();
    plan.display_name = std::string(257, 'x');
    REQUIRE_THROWS_WITH(
        arena::domain::MatchPlanner::normalize_and_schedule(plan),
        "display_name must be 1..256 bounded UTF-8 text bytes");

    plan = base_plan();
    plan.display_name =
        std::string("line") + "\xe2\x80\xa8" + "separator";
    REQUIRE_THROWS_WITH(
        arena::domain::MatchPlanner::normalize_and_schedule(plan),
        "display_name must be 1..256 bounded UTF-8 text bytes");
}

TEST_CASE("submitted pairings are discarded and regenerated") {
    auto plan = base_plan();
    plan.pairings.push_back(arena::domain::GamePairing{
        999,
        "attacker-white",
        "attacker-black",
        999,
        999,
        999,
    });
    const auto scheduled =
        arena::domain::MatchPlanner::normalize_and_schedule(plan);
    REQUIRE(scheduled.pairings.size() == 6);
    REQUIRE(scheduled.pairings.front().sequence == 1);
    REQUIRE(scheduled.pairings.front().white_engine_id != "attacker-white");
}

TEST_CASE("opening index addition cannot wrap the seed") {
    auto plan = base_plan();
    plan.engine_ids.pop_back();
    plan.format = arena::domain::TournamentFormat::head_to_head;
    plan.games_per_pair = 4;
    plan.seed = (std::numeric_limits<std::uint64_t>::max)();
    REQUIRE_THROWS_WITH(
        arena::domain::MatchPlanner::normalize_and_schedule(plan),
        "opening index overflows the planning domain");
}

TEST_CASE("planning-owned metadata is admitted before copying") {
    auto plan = base_plan();
    plan.id = "not-a-uuid";
    REQUIRE_THROWS_WITH(
        arena::domain::MatchPlanner::normalize_and_schedule(plan),
        "plan id must be a UUID");

    plan = base_plan();
    plan.opening_suite = std::filesystem::path{};
    REQUIRE_THROWS_WITH(
        arena::domain::MatchPlanner::normalize_and_schedule(plan),
        "opening suite path is invalid or too large");
}
