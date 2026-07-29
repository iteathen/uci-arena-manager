#pragma once

#include <cstdint>
#include <filesystem>
#include <string>

namespace arena::cli {

[[nodiscard]] std::string make_ui_picker_token();

// Runs until the process is interrupted. The listener is loopback-only and
// accepts picker requests carrying the exact per-process bearer token.
void run_ui_picker(
    std::uint16_t port,
    const std::string& bearer_token,
    const std::string& dashboard_origin,
    const std::filesystem::path& manager_data_root);

}  // namespace arena::cli
