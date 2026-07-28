#pragma once

#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace arena::domain {

using Labels = std::map<std::string, std::string>;

enum class ResourceKind {
    host,
    engine,
    bot,
    managed_service,
    health_probe,
    match_plan,
    tournament,
    game,
    operation,
    report,
    artifact,
    secret_reference,
    log_source,
    health_snapshot,
};

enum class TournamentFormat {
    head_to_head,
    round_robin,
    gauntlet,
    swiss,
    sprt,
};

enum class ChessVariant {
    standard,
    chess960,
};

struct ResourceLimits {
    std::optional<std::uint32_t> cpu_percent;
    std::optional<std::uint64_t> memory_bytes;
    std::uint32_t graceful_stop_timeout_ms{5'000};
};

struct UciOption {
    std::string name;
    std::string type;
    std::string default_value;
    std::optional<std::int64_t> minimum;
    std::optional<std::int64_t> maximum;
    std::vector<std::string> variants;
};

struct EngineDefinition {
    std::string id;
    std::string display_name;
    std::filesystem::path executable;
    std::filesystem::path working_directory;
    std::vector<std::string> arguments;
    std::map<std::string, std::string> option_overrides;
    std::vector<UciOption> discovered_options;
    std::string engine_name;
    std::string engine_author;
    std::string fingerprint;
    ResourceLimits limits;
    Labels labels;
};

struct TimeControl {
    std::uint64_t initial_ms{60'000};
    std::uint64_t increment_ms{600};
    std::optional<std::uint32_t> moves_to_go;
};

struct GamePairing {
    std::uint32_t sequence{};
    std::string white_engine_id;
    std::string black_engine_id;
    std::uint32_t round{};
    std::uint32_t board{};
    std::uint64_t opening_index{};
};

struct MatchPlan {
    std::string id;
    std::string display_name;
    TournamentFormat format{TournamentFormat::head_to_head};
    ChessVariant variant{ChessVariant::standard};
    std::vector<std::string> engine_ids;
    std::map<std::string, std::map<std::string, std::string>> engine_option_overrides;
    std::uint32_t games_per_pair{2};
    std::uint32_t rounds{1};
    std::uint32_t concurrency{1};
    std::uint64_t seed{};
    TimeControl time_control;
    std::optional<std::filesystem::path> opening_suite;
    std::vector<GamePairing> pairings;
    Labels labels;
};

[[nodiscard]] std::string to_string(ResourceKind value);
[[nodiscard]] std::string to_string(TournamentFormat value);
[[nodiscard]] std::string to_string(ChessVariant value);
[[nodiscard]] std::optional<TournamentFormat> tournament_format_from_string(const std::string& value);
[[nodiscard]] std::optional<ChessVariant> chess_variant_from_string(const std::string& value);

}  // namespace arena::domain
