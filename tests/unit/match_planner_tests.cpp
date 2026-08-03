#include "arena/domain/match_planner.hpp"

#include <catch2/catch_test_macros.hpp>

#include <limits>
#include <set>
#include <string>

namespace {

arena::domain::MatchPlan base_plan() {
    arena::domain::MatchPlan plan;
    plan.id = "018f218e-7be1-7a56-9daa-3f003641c801";
    plan.display_name = "Registered engine round robin";
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
    plan.format = arena::domain::TournamentFormat::round_robin;
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

TEST_CASE("time control and adjudication policy enforce contract bounds") {
    auto plan = base_plan();
    plan.time_control.moves_to_go = 0;
    REQUIRE_THROWS(
        arena::domain::MatchPlanner::normalize_and_schedule(plan));

    plan.time_control.moves_to_go = 40;
    plan.adjudication.max_plies = 2049;
    REQUIRE_THROWS(
        arena::domain::MatchPlanner::normalize_and_schedule(plan));

    plan.adjudication.max_plies = 512;
    plan.adjudication.draw_cp = 1001;
    REQUIRE_THROWS(
        arena::domain::MatchPlanner::normalize_and_schedule(plan));

    plan.adjudication.draw_cp.reset();
    plan.adjudication.win_consecutive_plies = 0;
    REQUIRE_THROWS(
        arena::domain::MatchPlanner::normalize_and_schedule(plan));
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
    plan.format = arena::domain::TournamentFormat::round_robin;
    plan.games_per_pair = 1;
    for (int index = 0; index < 150; ++index) {
        plan.engine_ids.push_back("engine-" + std::to_string(index));
    }
    REQUIRE_THROWS_WITH(
        arena::domain::MatchPlanner::normalize_and_schedule(plan),
        "competition schedule must contain 1..10000 games");
}

TEST_CASE("concurrency is bounded by capacity and the admission surface") {
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
}

TEST_CASE("engine option overrides are owned by bounded plan engines") {
    auto plan = base_plan();
    plan.engine_option_overrides["outside-engine"]["Threads"] = "4";
    REQUIRE_THROWS_WITH(
        arena::domain::MatchPlanner::normalize_and_schedule(plan),
        "engine option overrides name an engine outside the plan");

    plan.engine_option_overrides.clear();
    plan.engine_option_overrides[plan.engine_ids.front()]["Bad\nOption"] = "4";
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
    const auto normalized =
        arena::domain::MatchPlanner::normalize_and_schedule(plan);
    REQUIRE(normalized.display_name == "Local head_to_head");

    plan = base_plan();
    plan.display_name = std::string(257, 'x');
    REQUIRE_THROWS_WITH(
        arena::domain::MatchPlanner::normalize_and_schedule(plan),
        "display_name must be 1..256 bounded text bytes");
}
