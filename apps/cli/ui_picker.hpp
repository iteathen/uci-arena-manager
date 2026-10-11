#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <optional>
#include "arena/application/manager_installation_roots.hpp"

namespace arena::cli {

[[nodiscard]] std::string make_ui_picker_token();

// Runs until the process is interrupted. The listener is loopback-only and
// accepts picker requests carrying the exact per-process bearer token.
void run_ui_picker(
    std::uint16_t port,
    const std::string& bearer_token,
    const std::string& dashboard_origin,
    const std::filesystem::path& manager_data_root,
    const std::filesystem::path& setup_executable,
    bool allow_setup,
    const std::optional<arena::application::ManagerInstallationRoots>& installation_roots);

}  // namespace arena::cli
