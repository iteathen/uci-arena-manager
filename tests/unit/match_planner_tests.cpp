#include "arena/domain/match_planner.hpp"

#include <catch2/catch_test_macros.hpp>

#include <set>

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
    const auto scheduled = arena::domain::MatchPlanner::normalize_and_schedule(plan);
    REQUIRE(scheduled.pairings.size() == 6);
    for (std::size_t index = 0; index < scheduled.pairings.size(); index += 2) {
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
    REQUIRE_THROWS(arena::domain::MatchPlanner::normalize_and_schedule(plan));
    plan.engine_ids.pop_back();
    const auto scheduled = arena::domain::MatchPlanner::normalize_and_schedule(plan);
    REQUIRE(scheduled.pairings.size() == 2);
}

TEST_CASE("Swiss first round is deterministic for a seed") {
    auto plan = base_plan();
    plan.engine_ids.push_back("018f218e-7be1-7a56-9daa-3f003641c814");
    plan.format = arena::domain::TournamentFormat::swiss;
    const auto first = arena::domain::MatchPlanner::normalize_and_schedule(plan);
    const auto second = arena::domain::MatchPlanner::normalize_and_schedule(plan);
    REQUIRE(first.pairings.size() == 2);
    REQUIRE(first.pairings.front().white_engine_id == second.pairings.front().white_engine_id);
    REQUIRE(first.pairings.front().black_engine_id == second.pairings.front().black_engine_id);
}

TEST_CASE("color-paired opening indices remain aligned") {
    auto plan = base_plan();
    plan.engine_ids.pop_back();
    plan.format = arena::domain::TournamentFormat::head_to_head;
    plan.games_per_pair = 4;
    const auto scheduled = arena::domain::MatchPlanner::normalize_and_schedule(plan);
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
    REQUIRE_THROWS(arena::domain::MatchPlanner::normalize_and_schedule(plan));

    plan.games_per_pair = 4;
    plan.sprt.elo0 = 10;
    plan.sprt.elo1 = 10;
    REQUIRE_THROWS(arena::domain::MatchPlanner::normalize_and_schedule(plan));

    plan.sprt.elo1 = 20;
    plan.sprt.alpha = 0.5;
    REQUIRE_THROWS(arena::domain::MatchPlanner::normalize_and_schedule(plan));
}

TEST_CASE("time control and adjudication policy enforce contract bounds") {
    auto plan = base_plan();
    plan.time_control.moves_to_go = 0;
    REQUIRE_THROWS(arena::domain::MatchPlanner::normalize_and_schedule(plan));

    plan.time_control.moves_to_go = 40;
    plan.adjudication.max_plies = 2049;
    REQUIRE_THROWS(arena::domain::MatchPlanner::normalize_and_schedule(plan));

    plan.adjudication.max_plies = 512;
    plan.adjudication.draw_cp = 1001;
    REQUIRE_THROWS(arena::domain::MatchPlanner::normalize_and_schedule(plan));

    plan.adjudication.draw_cp.reset();
    plan.adjudication.win_consecutive_plies = 0;
    REQUIRE_THROWS(arena::domain::MatchPlanner::normalize_and_schedule(plan));
}
