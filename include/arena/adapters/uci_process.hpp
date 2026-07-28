#pragma once

#include "arena/domain/types.hpp"

#include <chrono>
#include <filesystem>
#include <string>
#include <vector>

namespace arena::adapters {

struct UciProbeResult {
    bool ok{false};
    std::string engine_name;
    std::string engine_author;
    std::string fingerprint;
    std::vector<domain::UciOption> options;
    std::string bestmove;
    std::uint64_t elapsed_ms{};
    std::uint64_t dropped_info_lines{};
    std::string error;
};

class UciProcess final {
  public:
    [[nodiscard]] static UciProbeResult probe(
        const std::filesystem::path& executable,
        const std::filesystem::path& working_directory,
        const std::vector<std::string>& arguments,
        std::chrono::milliseconds timeout = std::chrono::seconds(10));
};

}  // namespace arena::adapters
