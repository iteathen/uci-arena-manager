#pragma once

#include "arena/domain/types.hpp"

#include <chrono>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace arena::adapters {

struct UciRuntimeStatus final {
    std::string timing_contract_version;
    std::string model_id;
    std::string model_fingerprint;
    std::string model_class;
    std::string model_io_contract_fingerprint;
    std::string adapter_id;
    std::string provider_requested;
    std::string provider_effective;
    std::string provider_numeric_fingerprint;
    std::string hardware_driver_fingerprint;
    std::string onnx_runtime_version;
    std::string search_core;
};

struct UciProbeResult {
    bool ok{false};
    std::string engine_name;
    std::string engine_author;
    std::string fingerprint;
    std::string runtime_model_fingerprint;
    std::string runtime_provider;
    std::string runtime_provider_fingerprint;
    std::optional<UciRuntimeStatus> runtime_status;
    std::vector<domain::UciOption> options;
    std::string bestmove;
    std::uint64_t elapsed_ms{};
    std::uint64_t dropped_info_lines{};
    std::string error;
};

struct UciProbeTimeouts final {
    std::chrono::milliseconds uci_discovery{std::chrono::seconds(10)};
    std::chrono::milliseconds ready_after_options{std::chrono::seconds(60)};
    std::chrono::milliseconds runtime_status{std::chrono::seconds(10)};
    std::chrono::milliseconds first_bestmove{std::chrono::seconds(60)};
};

class UciProcess final {
  public:
    static void validate_option_overrides(
        const std::vector<domain::UciOption>& advertised_options,
        const std::map<std::string, std::string>& option_overrides);

    [[nodiscard]] static UciProbeResult probe(
        const std::filesystem::path& executable,
        const std::filesystem::path& working_directory,
        const std::vector<std::string>& arguments,
        const UciProbeTimeouts& timeouts = {},
        const std::map<std::string, std::string>& option_overrides = {},
        bool request_runtime_status = false);
};

}  // namespace arena::adapters
