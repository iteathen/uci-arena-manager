#pragma once

#include <chrono>
#include <cstddef>
#include <filesystem>
#include <nlohmann/json.hpp>
#include <stop_token>
#include <vector>

namespace arena::application {

struct EngineDiscoveryOptions final {
    std::size_t max_explicit_roots{8};
    std::size_t max_depth_below_root{1};
    std::size_t max_total_entries{4096};
    std::size_t max_metadata_calls{8192};
    std::size_t max_candidates{512};
    std::chrono::milliseconds max_elapsed{2000};
    std::size_t max_diagnostics{16};
    std::stop_token cancellation{};
};

class EngineOnboarding final {
  public:
    [[nodiscard]] static nlohmann::json discover(
        const std::filesystem::path& manager_data_root,
        const std::vector<std::filesystem::path>& explicit_roots = {},
        bool include_path_candidates = true,
        const EngineDiscoveryOptions& options = {});
};

}  // namespace arena::application
