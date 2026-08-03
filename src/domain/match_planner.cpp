#include "arena/domain/match_planner.hpp"

#include "arena/domain/uuid.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <string_view>
#include <unordered_set>

namespace arena::domain {
namespace {

constexpr std::uint64_t maximum_scheduled_games = 10'000;
constexpr std::uint32_t maximum_concurrency = 256;
constexpr std::size_t maximum_resource_id_bytes = 128;
constexpr std::size_t maximum_display_name_bytes = 256;
constexpr std::size_t maximum_engine_id_bytes = 128;
constexpr std::size_t maximum_overrides_per_engine = 128;
constexpr std::size_t maximum_total_overrides = 16'384;
constexpr std::size_t maximum_option_name_bytes = 128;
constexpr std::size_t maximum_option_value_bytes = 512;
constexpr std::size_t maximum_labels = 64;
constexpr std::size_t maximum_label_name_bytes = 128;
constexpr std::size_t maximum_label_value_bytes = 512;
constexpr std::size_t maximum_opening_suite_path_units = 32'767;
constexpr std::uint64_t deadline_reserve_ms = 2'000;
constexpr std::uint64_t maximum_chrono_clock_ms =
    static_cast<std::uint64_t>((std::numeric_limits<std::int64_t>::max)()) -
    deadline_reserve_ms;

[[nodiscard]] bool valid_utf8_scalar(
    std::string_view value,
    std::size_t maximum,
    bool allow_empty = false) {
    if ((!allow_empty && value.empty()) || value.size() > maximum) {
        return false;
    }

    std::size_t index = 0;
    while (index < value.size()) {
        const auto first = static_cast<unsigned char>(value[index]);
        std::uint32_t code_point = 0;
        std::size_t length = 0;
        std::uint32_t minimum = 0;

        if (first <= 0x7fU) {
            code_point = first;
            length = 1;
        } else if (first >= 0xc2U && first <= 0xdfU) {
            code_point = first & 0x1fU;
            length = 2;
            minimum = 0x80U;
        } else if (first >= 0xe0U && first <= 0xefU) {
            code_point = first & 0x0fU;
            length = 3;
            minimum = 0x800U;
        } else if (first >= 0xf0U && first <= 0xf4U) {
            code_point = first & 0x07U;
            length = 4;
            minimum = 0x10000U;
        } else {
            return false;
        }

        if (index + length > value.size()) {
            return false;
        }
        for (std::size_t offset = 1; offset < length; ++offset) {
            const auto continuation =
                static_cast<unsigned char>(value[index + offset]);
            if ((continuation & 0xc0U) != 0x80U) {
                return false;
            }
            code_point = (code_point << 6U) | (continuation & 0x3fU);
        }

        if ((length > 1 && code_point < minimum) ||
            code_point > 0x10ffffU ||
            (code_point >= 0xd800U && code_point <= 0xdfffU) ||
            code_point <= 0x1fU ||
            (code_point >= 0x7fU && code_point <= 0x9fU) ||
            code_point == 0x2028U ||
            code_point == 0x2029U) {
            return false;
        }
        index += length;
    }
    return true;
}

[[nodiscard]] std::uint64_t checked_multiply(
    std::uint64_t left,
    std::uint64_t right,
    const char* label) {
    if (left != 0 &&
        right > (std::numeric_limits<std::uint64_t>::max)() / left) {
        throw PlanError(std::string(label) + " overflows the planning domain");
    }
    return left * right;
}

[[nodiscard]] std::uint64_t checked_add(
    std::uint64_t left,
    std::uint64_t right,
    const char* label) {
    if (right > (std::numeric_limits<std::uint64_t>::max)() - left) {
        throw PlanError(std::string(label) + " overflows the planning domain");
    }
    return left + right;
}

struct ScheduleCapacity {
    std::uint64_t complete_games{};
    std::uint64_t immediately_admittable_games{};
    std::uint64_t maximum_opening_offset{};
};

[[nodiscard]] ScheduleCapacity schedule_capacity(const MatchPlan& plan) {
    const auto engines = static_cast<std::uint64_t>(plan.engine_ids.size());
    if (engines < 2) {
        throw PlanError("a local match plan requires at least two engines");
    }
    if (plan.games_per_pair == 0) {
        throw PlanError("games_per_pair must be positive");
    }
    if ((plan.format == TournamentFormat::head_to_head ||
         plan.format == TournamentFormat::sprt) &&
        engines != 2) {
        throw PlanError(
            "head-to-head and SPRT plans require exactly two engines");
    }
    if (plan.format == TournamentFormat::swiss && plan.rounds == 0) {
        throw PlanError("Swiss plans require at least one round");
    }

    std::uint64_t complete = 0;
    std::uint64_t immediately_admittable = 0;
    std::uint64_t maximum_opening_offset = 0;
    switch (plan.format) {
        case TournamentFormat::head_to_head:
        case TournamentFormat::sprt:
            complete = plan.games_per_pair;
            immediately_admittable = complete;
            maximum_opening_offset = (complete - 1U) / 2U;
            break;
        case TournamentFormat::round_robin: {
            const auto pairs =
                checked_multiply(
                    engines,
                    engines - 1,
                    "round-robin pair count") /
                2;
            complete = checked_multiply(
                pairs,
                plan.games_per_pair,
                "round-robin schedule");
            immediately_admittable = complete;
            maximum_opening_offset = (complete - 1U) / 2U;
            break;
        }
        case TournamentFormat::gauntlet:
            complete = checked_multiply(
                engines - 1,
                plan.games_per_pair,
                "gauntlet schedule");
            immediately_admittable = complete;
            maximum_opening_offset = (complete - 1U) / 2U;
            break;
        case TournamentFormat::swiss:
            immediately_admittable = engines / 2;
            complete = checked_multiply(
                immediately_admittable,
                plan.rounds,
                "Swiss schedule");
            maximum_opening_offset = immediately_admittable - 1U;
            break;
    }
    if (complete == 0 || complete > maximum_scheduled_games) {
        throw PlanError("competition schedule must contain 1..10000 games");
    }
    static_cast<void>(checked_add(
        plan.seed,
        maximum_opening_offset,
        "opening index"));
    return {complete, immediately_admittable, maximum_opening_offset};
}

void validate_clock_representation(const MatchPlan& plan) {
    if (plan.time_control.initial_ms > maximum_chrono_clock_ms ||
        plan.time_control.increment_ms > maximum_chrono_clock_ms) {
        throw PlanError(
            "time control exceeds signed chrono millisecond capacity");
    }
    const std::uint64_t maximum_moves_per_side =
        (static_cast<std::uint64_t>(plan.adjudication.max_plies) + 1U) / 2U;
    if (plan.time_control.increment_ms != 0 &&
        maximum_moves_per_side >
            (maximum_chrono_clock_ms - plan.time_control.initial_ms) /
                plan.time_control.increment_ms) {
        throw PlanError("time control can overflow before max_plies");
    }
}

[[nodiscard]] MatchPlan admitted_plan_copy(
    const MatchPlan& plan,
    std::string_view normalized_display_name) {
    MatchPlan admitted;
    admitted.id = plan.id.empty() ? uuid_v7() : plan.id;
    admitted.display_name.assign(normalized_display_name);
    admitted.format = plan.format;
    admitted.variant = plan.variant;
    admitted.engine_ids = plan.engine_ids;
    admitted.engine_option_overrides = plan.engine_option_overrides;
    admitted.games_per_pair = plan.games_per_pair;
    admitted.rounds = plan.rounds;
    admitted.concurrency = plan.concurrency;
    admitted.seed = plan.seed;
    admitted.time_control = plan.time_control;
    admitted.adjudication = plan.adjudication;
    admitted.sprt = plan.sprt;
    admitted.opening_suite = plan.opening_suite;
    admitted.labels = plan.labels;
    return admitted;
}

}  // namespace

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

void MatchPlanner::validate(
    const MatchPlan& plan,
    const std::string_view normalized_display_name,
    const std::uint64_t scheduled_games,
    const std::uint64_t immediately_admittable_games) {
    if (!valid_utf8_scalar(plan.id, maximum_resource_id_bytes, true)) {
        throw PlanError("plan id must be bounded UTF-8 scalar text");
    }
    if (!valid_utf8_scalar(
            normalized_display_name,
            maximum_display_name_bytes)) {
        throw PlanError(
            "display_name must be 1..256 bounded UTF-8 text bytes");
    }

    std::unordered_set<std::string_view> unique;
    unique.reserve(plan.engine_ids.size());
    for (const auto& engine_id : plan.engine_ids) {
        if (!valid_utf8_scalar(engine_id, maximum_engine_id_bytes)) {
            throw PlanError("engine IDs must be bounded UTF-8 scalar text");
        }
        if (!unique.insert(std::string_view(engine_id)).second) {
            throw PlanError(
                "engine IDs in a local match plan must be distinct");
        }
    }

    std::size_t total_overrides = 0;
    for (const auto& [engine_id, overrides] :
         plan.engine_option_overrides) {
        if (!unique.contains(std::string_view(engine_id))) {
            throw PlanError(
                "engine option overrides name an engine outside the plan");
        }
        if (overrides.size() > maximum_overrides_per_engine) {
            throw PlanError("engine option override count exceeds 128");
        }
        if (overrides.size() >
            maximum_total_overrides - total_overrides) {
            throw PlanError(
                "total engine option override count exceeds 16384");
        }
        total_overrides += overrides.size();
        for (const auto& [name, value] : overrides) {
            if (!valid_utf8_scalar(name, maximum_option_name_bytes) ||
                !valid_utf8_scalar(
                    value,
                    maximum_option_value_bytes,
                    true)) {
                throw PlanError(
                    "engine option override is invalid or too large");
            }
        }
    }

    if (plan.concurrency == 0 ||
        plan.concurrency > maximum_concurrency) {
        throw PlanError("concurrency must be between 1 and 256");
    }
    if (plan.concurrency > immediately_admittable_games) {
        throw PlanError(
            "concurrency cannot exceed the currently admittable game count");
    }
    if (scheduled_games == 0 ||
        scheduled_games > maximum_scheduled_games) {
        throw PlanError("competition schedule must contain 1..10000 games");
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
        throw PlanError("adjudication consecutive-ply bounds are invalid");
    }

    if (plan.format == TournamentFormat::sprt) {
        if (plan.games_per_pair % 2 != 0) {
            throw PlanError(
                "SPRT maximum games must be even for color pairing");
        }
        if (!std::isfinite(plan.sprt.elo0) ||
            !std::isfinite(plan.sprt.elo1) ||
            !std::isfinite(plan.sprt.alpha) ||
            !std::isfinite(plan.sprt.beta)) {
            throw PlanError("SPRT values must be finite");
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

    if (plan.labels.size() > maximum_labels) {
        throw PlanError("plan label count exceeds 64");
    }
    for (const auto& [name, value] : plan.labels) {
        if (!valid_utf8_scalar(name, maximum_label_name_bytes) ||
            !valid_utf8_scalar(
                value,
                maximum_label_value_bytes,
                true)) {
            throw PlanError("plan label is invalid or too large");
        }
    }
    if (plan.opening_suite) {
        const auto& native = plan.opening_suite->native();
        if (native.empty() ||
            native.size() > maximum_opening_suite_path_units ||
            std::find(native.begin(), native.end(),
                      std::filesystem::path::value_type{}) != native.end()) {
            throw PlanError("opening suite path is invalid or too large");
        }
    }

    validate_clock_representation(plan);
}

MatchPlan MatchPlanner::normalize_and_schedule(const MatchPlan& plan) {
    const ScheduleCapacity capacity = schedule_capacity(plan);
    std::string generated_display_name;
    const std::string_view normalized_display_name = plan.display_name.empty()
        ? std::string_view(
              generated_display_name = "Local " + to_string(plan.format))
        : std::string_view(plan.display_name);
    validate(
        plan,
        normalized_display_name,
        capacity.complete_games,
        capacity.immediately_admittable_games);

    MatchPlan scheduled = admitted_plan_copy(plan, normalized_display_name);
    scheduled.pairings.reserve(static_cast<std::size_t>(
        scheduled.format == TournamentFormat::swiss
            ? capacity.immediately_admittable_games
            : capacity.complete_games));

    switch (scheduled.format) {
        case TournamentFormat::head_to_head:
        case TournamentFormat::sprt:
            schedule_head_to_head(scheduled);
            break;
        case TournamentFormat::round_robin:
            schedule_round_robin(scheduled);
            break;
        case TournamentFormat::gauntlet:
            schedule_gauntlet(scheduled);
            break;
        case TournamentFormat::swiss:
            schedule_swiss_first_round(scheduled);
            break;
    }
    return scheduled;
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
            checked_add(plan.seed, game / 2, "opening index"),
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
                const std::uint64_t opening_offset =
                    (static_cast<std::uint64_t>(sequence) - 1U) / 2U;
                plan.pairings.push_back(GamePairing{
                    sequence++,
                    plan.engine_ids[reverse ? second : first],
                    plan.engine_ids[reverse ? first : second],
                    round,
                    static_cast<std::uint32_t>(second),
                    checked_add(
                        plan.seed,
                        opening_offset,
                        "opening index"),
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
            const std::uint64_t opening_offset =
                (static_cast<std::uint64_t>(sequence) - 1U) / 2U;
            plan.pairings.push_back(GamePairing{
                sequence++,
                plan.engine_ids[reverse ? challenger : 0],
                plan.engine_ids[reverse ? 0 : challenger],
                static_cast<std::uint32_t>(challenger),
                1,
                checked_add(
                    plan.seed,
                    opening_offset,
                    "opening index"),
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
            ((index / 2U) & 1U) != (plan.seed & 1U);
        plan.pairings.push_back(GamePairing{
            sequence++,
            seeded[index + (reverse ? 1U : 0U)],
            seeded[index + (reverse ? 0U : 1U)],
            1,
            static_cast<std::uint32_t>((index / 2U) + 1U),
            checked_add(plan.seed, index / 2U, "opening index"),
        });
    }
}

}  // namespace arena::domain
