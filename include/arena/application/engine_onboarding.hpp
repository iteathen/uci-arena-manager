#pragma once

#include <chrono>
#include <cstddef>
#include <filesystem>
#include <nlohmann/json.hpp>
#include <optional>
#include <stop_token>
#include <string>
#include <vector>

namespace arena::adapters {
struct UciRuntimeStatus;
}

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
    enum class TimingModelCompatibility {
        exact_model,
        class_io,
    };

    [[nodiscard]] static nlohmann::json discover(
        const std::filesystem::path& manager_data_root,
        const std::vector<std::filesystem::path>& explicit_roots = {},
        bool include_path_candidates = true,
        const EngineDiscoveryOptions& options = {});

    [[nodiscard]] static std::optional<std::string>
    runtime_identity_mismatch(
        const nlohmann::json& expected,
        const adapters::UciRuntimeStatus& observed,
        TimingModelCompatibility model_compatibility =
            TimingModelCompatibility::exact_model);
};

}  // namespace arena::application
