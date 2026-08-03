#include "arena/domain/match_planner.hpp"

#include "arena/domain/uuid.hpp"

#include <algorithm>
#include <cctype>
#include <limits>
#include <set>
#include <string_view>

namespace arena::domain {
namespace {

constexpr std::uint64_t maximum_scheduled_games = 10'000;
constexpr std::uint32_t maximum_concurrency = 256;
constexpr std::size_t maximum_display_name_bytes = 256;
constexpr std::size_t maximum_engine_id_bytes = 128;
constexpr std::size_t maximum_overrides_per_engine = 128;
constexpr std::size_t maximum_option_name_bytes = 128;
constexpr std::size_t maximum_option_value_bytes = 512;
constexpr std::uint64_t deadline_reserve_ms = 2'000;
constexpr std::uint64_t maximum_chrono_clock_ms =
    static_cast<std::uint64_t>((std::numeric_limits<std::int64_t>::max)()) -
    deadline_reserve_ms;

bool bounded_scalar(
    const std::string_view value,
    const std::size_t maximum,
    const bool allow_empty = false) {
    return (allow_empty || !value.empty()) && value.size() <= maximum &&
           std::ranges::none_of(value, [](const unsigned char character) {
               return character == 0 || character == '\r' || character == '\n' ||
                      (character < 0x20 && character != '\t') || character == 0x7f;
           });
}

std::uint64_t checked_multiply(
    const std::uint64_t left,
    const std::uint64_t right,
    const char* label) {
    if (left != 0 &&
        right > (std::numeric_limits<std::uint64_t>::max)() / left) {
        throw PlanError(std::string(label) + " overflows the planning domain");
    }
    return left * right;
}

std::uint64_t expected_scheduled_games(const MatchPlan& plan) {
    const std::uint64_t engines = plan.engine_ids.size();
    switch (plan.format) {
        case TournamentFormat::head_to_head:
        case TournamentFormat::sprt:
            return plan.games_per_pair;
        case TournamentFormat::round_robin: {
            const std::uint64_t pairs =
                checked_multiply(engines, engines - 1, "round-robin pair count") /
                2;
            return checked_multiply(
                pairs,
                plan.games_per_pair,
                "round-robin schedule");
        }
        case TournamentFormat::gauntlet:
            return checked_multiply(
                engines - 1,
                plan.games_per_pair,
                "gauntlet schedule");
        case TournamentFormat::swiss:
            return checked_multiply(
                engines / 2,
                plan.rounds,
                "Swiss schedule");
    }
    throw PlanError("unsupported tournament format");
}

std::uint64_t immediately_admittable_games(const MatchPlan& plan) {
    if (plan.format == TournamentFormat::swiss) {
        return plan.engine_ids.size() / 2;
    }
    return expected_scheduled_games(plan);
}

void validate_clock_representation(const MatchPlan& plan) {
    if (plan.time_control.initial_ms > maximum_chrono_clock_ms ||
        plan.time_control.increment_ms > maximum_chrono_clock_ms) {
        throw PlanError(
            "time control exceeds signed chrono millisecond capacity");
    }
    const std::uint64_t maximum_moves_per_side =
        (static_cast<std::uint64_t>(plan.adjudication.max_plies) + 1) / 2;
    if (plan.time_control.increment_ms != 0 &&
        maximum_moves_per_side >
            (maximum_chrono_clock_ms - plan.time_control.initial_ms) /
                plan.time_control.increment_ms) {
        throw PlanError(
            "time control can overflow before max_plies");
    }
}

} // namespace

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

std::optional<TournamentFormat> tournament_format_from_string(
    const std::string& value) {
    if (value == "head_to_head" || value == "match") {
        return TournamentFormat::head_to_head;
    }
    if (value == "round_robin") return TournamentFormat::round_robin;
    if (value == "gauntlet") return TournamentFormat::gauntlet;
    if (value == "swiss") return TournamentFormat::swiss;
    if (value == "sprt") return TournamentFormat::sprt;
    return std::nullopt;
}

std::optional<ChessVariant> chess_variant_from_string(
    const std::string& value) {
    if (value == "standard") return ChessVariant::standard;
    if (value == "chess960") return ChessVariant::chess960;
    return std::nullopt;
}

void MatchPlanner::validate(const MatchPlan& plan) {
    if (!bounded_scalar(plan.display_name, maximum_display_name_bytes)) {
        throw PlanError("display_name must be 1..256 bounded text bytes");
    }
    if (plan.engine_ids.size() < 2) {
        throw PlanError("a local match plan requires at least two engines");
    }
    for (const auto& engine_id : plan.engine_ids) {
        if (!bounded_scalar(engine_id, maximum_engine_id_bytes)) {
            throw PlanError("engine IDs must be bounded scalar text");
        }
    }
    const std::set<std::string> unique(
        plan.engine_ids.begin(), plan.engine_ids.end());
    if (unique.size() != plan.engine_ids.size()) {
        throw PlanError("engine IDs in a local match plan must be distinct");
    }
    for (const auto& [engine_id, overrides] :
         plan.engine_option_overrides) {
        if (!unique.contains(engine_id)) {
            throw PlanError(
                "engine option overrides name an engine outside the plan");
        }
        if (overrides.size() > maximum_overrides_per_engine) {
            throw PlanError(
                "engine option override count exceeds 128");
        }
        for (const auto& [name, value] : overrides) {
            if (!bounded_scalar(name, maximum_option_name_bytes) ||
                !bounded_scalar(
                    value,
                    maximum_option_value_bytes,
                    true)) {
                throw PlanError(
                    "engine option override is invalid or too large");
            }
        }
    }
    if (plan.games_per_pair == 0) {
        throw PlanError("games_per_pair must be positive");
    }
    if (plan.concurrency == 0 ||
        plan.concurrency > maximum_concurrency) {
        throw PlanError("concurrency must be between 1 and 256");
    }
    if (plan.time_control.initial_ms == 0) {
        throw PlanError("time control initial_ms must be positive");
    }
    if (plan.time_control.moves_to_go &&
        *plan.time_control.moves_to_go == 0) {
        throw PlanError("time control moves_to_go must be positive");
    }
    if (plan.adjudication.max_plies < 2 ||
        plan.adjudication.max_plies > 2048) {
        throw PlanError(
            "adjudication max_plies must be between 2 and 2048");
    }
    if (plan.adjudication.draw_cp &&
        (*plan.adjudication.draw_cp < 0 ||
         *plan.adjudication.draw_cp > 1000)) {
        throw PlanError(
            "adjudication draw_cp must be between 0 and 1000");
    }
    if (plan.adjudication.win_cp &&
        (*plan.adjudication.win_cp < 1 ||
         *plan.adjudication.win_cp > 100000)) {
        throw PlanError(
            "adjudication win_cp must be between 1 and 100000");
    }
    if (plan.adjudication.draw_consecutive_plies == 0 ||
        plan.adjudication.draw_consecutive_plies > 100 ||
        plan.adjudication.win_consecutive_plies == 0 ||
        plan.adjudication.win_consecutive_plies > 100 ||
        plan.adjudication.draw_min_plies > 1000) {
        throw PlanError(
            "adjudication consecutive-ply bounds are invalid");
    }
    if ((plan.format == TournamentFormat::head_to_head ||
         plan.format == TournamentFormat::sprt) &&
        plan.engine_ids.size() != 2) {
        throw PlanError(
            "head-to-head and SPRT plans require exactly two engines");
    }
    if (plan.format == TournamentFormat::gauntlet &&
        plan.engine_ids.size() < 2) {
        throw PlanError(
            "gauntlet plans require a champion and at least one challenger");
    }
    if (plan.format == TournamentFormat::swiss && plan.rounds == 0) {
        throw PlanError("Swiss plans require at least one round");
    }
    if (plan.format == TournamentFormat::sprt) {
        if (plan.games_per_pair % 2 != 0) {
            throw PlanError(
                "SPRT maximum games must be even for color pairing");
        }
        if (plan.sprt.elo0 < -1000.0 ||
            plan.sprt.elo0 > 1000.0 ||
            plan.sprt.elo1 < -1000.0 ||
            plan.sprt.elo1 > 1000.0 ||
            plan.sprt.elo1 <= plan.sprt.elo0) {
            throw PlanError(
                "SPRT requires -1000 <= elo0 < elo1 <= 1000");
        }
        if (plan.sprt.alpha <= 0.0 ||
            plan.sprt.alpha >= 0.5 ||
            plan.sprt.beta <= 0.0 ||
            plan.sprt.beta >= 0.5) {
            throw PlanError(
                "SPRT alpha and beta must be between 0 and 0.5");
        }
    }

    validate_clock_representation(plan);
    const std::uint64_t scheduled_games =
        expected_scheduled_games(plan);
    if (scheduled_games == 0 ||
        scheduled_games > maximum_scheduled_games) {
        throw PlanError(
            "competition schedule must contain 1..10000 games");
    }
    if (plan.concurrency > immediately_admittable_games(plan)) {
        throw PlanError(
            "concurrency cannot exceed the currently admittable game count");
    }
}

MatchPlan MatchPlanner::normalize_and_schedule(MatchPlan plan) {
    if (plan.id.empty()) {
        plan.id = uuid_v7();
    }
    if (plan.display_name.empty()) {
        plan.display_name = "Local " + to_string(plan.format);
    }
    validate(plan);
    plan.pairings.clear();
    plan.pairings.reserve(
        static_cast<std::size_t>(
            plan.format == TournamentFormat::swiss
                ? plan.engine_ids.size() / 2
                : expected_scheduled_games(plan)));

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
    for (std::uint32_t game = 0;
         game < plan.games_per_pair;
         ++game) {
        const bool reverse = game % 2 == 1;
        plan.pairings.push_back(GamePairing{
            game + 1,
            plan.engine_ids[reverse ? 1U : 0U],
            plan.engine_ids[reverse ? 0U : 1U],
            (game / 2) + 1,
            1,
            plan.seed + game / 2,
        });
    }
}

void MatchPlanner::schedule_round_robin(MatchPlan& plan) {
    std::uint32_t sequence = 1;
    std::uint32_t round = 1;
    for (std::size_t first = 0;
         first < plan.engine_ids.size();
         ++first) {
        for (std::size_t second = first + 1;
             second < plan.engine_ids.size();
             ++second) {
            for (std::uint32_t game = 0;
                 game < plan.games_per_pair;
                 ++game) {
                const bool reverse = game % 2 == 1;
                plan.pairings.push_back(GamePairing{
                    sequence++,
                    plan.engine_ids[reverse ? second : first],
                    plan.engine_ids[reverse ? first : second],
                    round,
                    static_cast<std::uint32_t>(second),
                    plan.seed + (sequence - 2) / 2,
                });
            }
            ++round;
        }
    }
}

void MatchPlanner::schedule_gauntlet(MatchPlan& plan) {
    std::uint32_t sequence = 1;
    for (std::size_t challenger = 1;
         challenger < plan.engine_ids.size();
         ++challenger) {
        for (std::uint32_t game = 0;
             game < plan.games_per_pair;
             ++game) {
            const bool reverse = game % 2 == 1;
            plan.pairings.push_back(GamePairing{
                sequence++,
                plan.engine_ids[reverse ? challenger : 0],
                plan.engine_ids[reverse ? 0 : challenger],
                static_cast<std::uint32_t>(challenger),
                1,
                plan.seed + (sequence - 2) / 2,
            });
        }
    }
}

void MatchPlanner::schedule_swiss_first_round(MatchPlan& plan) {
    std::vector<std::string> seeded = plan.engine_ids;
    std::rotate(
        seeded.begin(),
        seeded.begin() +
            static_cast<std::ptrdiff_t>(plan.seed % seeded.size()),
        seeded.end());
    std::uint32_t sequence = 1;
    for (std::size_t index = 0;
         index + 1 < seeded.size();
         index += 2) {
        const bool reverse =
            ((index / 2U) + plan.seed) % 2U == 1U;
        plan.pairings.push_back(GamePairing{
            sequence++,
            seeded[index + (reverse ? 1U : 0U)],
            seeded[index + (reverse ? 0U : 1U)],
            1,
            static_cast<std::uint32_t>((index / 2) + 1),
            plan.seed + index / 2U,
        });
    }
}

}  // namespace arena::domain
