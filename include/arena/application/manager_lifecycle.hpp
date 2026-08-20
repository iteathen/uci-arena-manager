#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

#include <nlohmann/json_fwd.hpp>

namespace arena::application {

inline constexpr std::string_view manager_readiness_schema =
    "arena_manager_readiness_v1";
inline constexpr std::chrono::milliseconds manager_core_readiness_deadline{
    120'000};
inline constexpr std::chrono::milliseconds manager_startup_poll_interval{25};

enum class ManagerStartupProbeState {
    pending,
    ready,
    failed,
};

struct ManagerStartupProbe {
    ManagerStartupProbeState state{ManagerStartupProbeState::pending};
    std::string detail;
};

enum class ManagerStartupWaitState {
    ready,
    service_exited,
    probe_failed,
    timed_out,
};

struct ManagerStartupWaitResult {
    ManagerStartupWaitState state{ManagerStartupWaitState::timed_out};
    std::string detail;
    std::optional<std::uint32_t> service_exit_code;
    std::size_t observations{};
    std::chrono::milliseconds elapsed{};
};

using ManagerStartupProbeFunction = std::function<ManagerStartupProbe()>;
using ManagerServiceExitProbe =
    std::function<std::optional<std::uint32_t>()>;
using ManagerStartupNow =
    std::function<std::chrono::steady_clock::time_point()>;
using ManagerStartupSleep = std::function<void(std::chrono::milliseconds)>;

[[nodiscard]] ManagerStartupWaitResult wait_for_manager_startup(
    const ManagerStartupProbeFunction& readiness_probe,
    const ManagerServiceExitProbe& service_exit_probe,
    const ManagerStartupNow& now,
    const ManagerStartupSleep& sleep,
    std::chrono::milliseconds deadline = manager_core_readiness_deadline,
    std::chrono::milliseconds poll_interval = manager_startup_poll_interval);

enum class DesktopStartupFailureCategory {
    service_launch,
    service_process_exit,
    service_readiness,
    pair_helper,
    webview_initialization,
};

enum class DesktopLaunchDisposition {
    start_authority,
    activate_existing,
};

[[nodiscard]] std::string_view desktop_startup_category(
    DesktopStartupFailureCategory category) noexcept;
[[nodiscard]] constexpr DesktopLaunchDisposition desktop_launch_disposition(
    const bool existing_instance) noexcept {
    return existing_instance
        ? DesktopLaunchDisposition::activate_existing
        : DesktopLaunchDisposition::start_authority;
}
[[nodiscard]] std::string bounded_startup_detail(std::string_view detail);
[[nodiscard]] std::string manager_readiness_error(
    const nlohmann::json& document,
    std::string_view expected_version);

}  // namespace arena::application
