#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace arena::adapters {

struct UciGameEngine {
    std::string resource_id;
    std::string fingerprint;
    std::filesystem::path executable;
    std::filesystem::path working_directory;
    std::vector<std::string> arguments;
    std::map<std::string, std::string> options;
};

struct UciGameState {
    std::vector<std::string> uci_moves;
    std::vector<std::string> san_moves;
    std::vector<std::string> per_ply_fen;
    std::vector<std::optional<std::int32_t>> evaluations_cp;
    std::vector<std::vector<std::string>> principal_variations;
    std::uint64_t white_time_ms{};
    std::uint64_t black_time_ms{};
};

struct UciGameRequest {
    UciGameEngine white;
    UciGameEngine black;
    std::uint64_t initial_time_ms{60'000};
    std::uint64_t increment_ms{600};
    std::optional<std::uint32_t> moves_to_go;
    std::string starting_fen{
        "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1"};
    bool chess960{};
    std::uint32_t max_plies{512};
    std::optional<std::int32_t> draw_cp;
    std::uint32_t draw_consecutive_plies{8};
    std::uint32_t draw_min_plies{80};
    std::optional<std::int32_t> win_cp;
    std::uint32_t win_consecutive_plies{6};
    UciGameState resume;
};

struct UciGameResult {
    std::string lifecycle{"failed"};
    std::string result{"*"};
    std::string termination{"protocol_failure"};
    std::string detail;
    UciGameState state;
};

class UciGameRunner final {
  public:
    using CancellationProbe = std::function<bool()>;
    using PlyCommit = std::function<void(const UciGameState&)>;

    [[nodiscard]] static UciGameResult play(
        const UciGameRequest& request,
        const CancellationProbe& cancelled,
        const PlyCommit& commit_ply);
};

}  // namespace arena::adapters
