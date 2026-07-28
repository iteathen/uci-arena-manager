#include "arena/domain/match_planner.hpp"

#include "arena/domain/uuid.hpp"

#include <algorithm>
#include <set>

namespace arena::domain {

std::string to_string(ResourceKind value) {
    switch (value) {
        case ResourceKind::host: return "host";
        case ResourceKind::engine: return "engine";
        case ResourceKind::bot: return "bot";
        case ResourceKind::managed_service: return "managed_service";
        case ResourceKind::health_probe: return "health_probe";
        case ResourceKind::match_plan: return "match_plan";
        case ResourceKind::tournament: return "tournament";
        case ResourceKind::game: return "game";
        case ResourceKind::operation: return "operation";
        case ResourceKind::report: return "report";
        case ResourceKind::artifact: return "artifact";
        case ResourceKind::secret_reference: return "secret_reference";
        case ResourceKind::log_source: return "log_source";
        case ResourceKind::health_snapshot: return "health_snapshot";
    }
    return "unknown";
}

std::string to_string(TournamentFormat value) {
    switch (value) {
        case TournamentFormat::head_to_head: return "head_to_head";
        case TournamentFormat::round_robin: return "round_robin";
        case TournamentFormat::gauntlet: return "gauntlet";
        case TournamentFormat::swiss: return "swiss";
        case TournamentFormat::sprt: return "sprt";
    }
    return "unknown";
}

std::string to_string(ChessVariant value) {
    return value == ChessVariant::chess960 ? "chess960" : "standard";
}

std::optional<TournamentFormat> tournament_format_from_string(const std::string& value) {
    if (value == "head_to_head" || value == "match") return TournamentFormat::head_to_head;
    if (value == "round_robin") return TournamentFormat::round_robin;
    if (value == "gauntlet") return TournamentFormat::gauntlet;
    if (value == "swiss") return TournamentFormat::swiss;
    if (value == "sprt") return TournamentFormat::sprt;
    return std::nullopt;
}

std::optional<ChessVariant> chess_variant_from_string(const std::string& value) {
    if (value == "standard") return ChessVariant::standard;
    if (value == "chess960") return ChessVariant::chess960;
    return std::nullopt;
}

void MatchPlanner::validate(const MatchPlan& plan) {
    if (plan.engine_ids.size() < 2) {
        throw PlanError("a local match plan requires at least two engines");
    }
    const std::set<std::string> unique(plan.engine_ids.begin(), plan.engine_ids.end());
    if (unique.size() != plan.engine_ids.size()) {
        throw PlanError("engine IDs in a local match plan must be distinct");
    }
    if (plan.games_per_pair == 0) {
        throw PlanError("games_per_pair must be positive");
    }
    if (plan.concurrency == 0) {
        throw PlanError("concurrency must be positive");
    }
    if (plan.time_control.initial_ms == 0) {
        throw PlanError("time control initial_ms must be positive");
    }
    if ((plan.format == TournamentFormat::head_to_head ||
         plan.format == TournamentFormat::sprt) &&
        plan.engine_ids.size() != 2) {
        throw PlanError("head-to-head and SPRT plans require exactly two engines");
    }
    if (plan.format == TournamentFormat::gauntlet && plan.engine_ids.size() < 2) {
        throw PlanError("gauntlet plans require a champion and at least one challenger");
    }
}

MatchPlan MatchPlanner::normalize_and_schedule(MatchPlan plan) {
    validate(plan);
    if (plan.id.empty()) {
        plan.id = uuid_v7();
    }
    if (plan.display_name.empty()) {
        plan.display_name = "Local " + to_string(plan.format);
    }
    plan.pairings.clear();

    switch (plan.format) {
        case TournamentFormat::head_to_head:
        case TournamentFormat::sprt:
            schedule_head_to_head(plan);
            break;
        case TournamentFormat::round_robin:
            schedule_round_robin(plan);
            break;
        case TournamentFormat::gauntlet:
            schedule_gauntlet(plan);
            break;
        case TournamentFormat::swiss:
            schedule_swiss_first_round(plan);
            break;
    }
    return plan;
}

void MatchPlanner::schedule_head_to_head(MatchPlan& plan) {
    for (std::uint32_t game = 0; game < plan.games_per_pair; ++game) {
        const bool reverse = game % 2 == 1;
        plan.pairings.push_back(GamePairing{
            game + 1,
            plan.engine_ids[reverse ? 1U : 0U],
            plan.engine_ids[reverse ? 0U : 1U],
            (game / 2) + 1,
            1,
            game / 2,
        });
    }
}

void MatchPlanner::schedule_round_robin(MatchPlan& plan) {
    std::uint32_t sequence = 1;
    std::uint32_t round = 1;
    for (std::size_t first = 0; first < plan.engine_ids.size(); ++first) {
        for (std::size_t second = first + 1; second < plan.engine_ids.size(); ++second) {
            for (std::uint32_t game = 0; game < plan.games_per_pair; ++game) {
                const bool reverse = game % 2 == 1;
                plan.pairings.push_back(GamePairing{
                    sequence++,
                    plan.engine_ids[reverse ? second : first],
                    plan.engine_ids[reverse ? first : second],
                    round,
                    static_cast<std::uint32_t>(second),
                    game / 2,
                });
            }
            ++round;
        }
    }
}

void MatchPlanner::schedule_gauntlet(MatchPlan& plan) {
    std::uint32_t sequence = 1;
    for (std::size_t challenger = 1; challenger < plan.engine_ids.size(); ++challenger) {
        for (std::uint32_t game = 0; game < plan.games_per_pair; ++game) {
            const bool reverse = game % 2 == 1;
            plan.pairings.push_back(GamePairing{
                sequence++,
                plan.engine_ids[reverse ? challenger : 0],
                plan.engine_ids[reverse ? 0 : challenger],
                static_cast<std::uint32_t>(challenger),
                1,
                game / 2,
            });
        }
    }
}

void MatchPlanner::schedule_swiss_first_round(MatchPlan& plan) {
    std::vector<std::string> seeded = plan.engine_ids;
    std::rotate(
        seeded.begin(),
        seeded.begin() + static_cast<std::ptrdiff_t>(plan.seed % seeded.size()),
        seeded.end());
    std::uint32_t sequence = 1;
    for (std::size_t index = 0; index + 1 < seeded.size(); index += 2) {
        plan.pairings.push_back(GamePairing{
            sequence++,
            seeded[index],
            seeded[index + 1],
            1,
            static_cast<std::uint32_t>((index / 2) + 1),
            0,
        });
    }
}

}  // namespace arena::domain
