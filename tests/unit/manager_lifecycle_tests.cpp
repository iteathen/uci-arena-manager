#include "arena/application/manager_lifecycle.hpp"

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

#include <array>
#include <chrono>
#include <optional>
#include <set>
#include <string>

namespace {

using namespace std::chrono_literals;
using arena::application::DesktopLaunchDisposition;
using arena::application::DesktopStartupFailureCategory;
using arena::application::ManagerStartupProbe;
using arena::application::ManagerStartupProbeState;
using arena::application::ManagerStartupWaitState;
using nlohmann::json;

json readiness_document(
    const std::string& service_state = "pending",
    const std::string& engine_state = "pending") {
    return {
        {"schema", "arena_manager_readiness_v1"},
        {"service", "uci-arena-manager"},
        {"version", "test-version"},
        {"state", "core_ready"},
        {"started_at", "2026-08-20T12:00:00Z"},
        {"installed_service_reconciliation",
         {{"schema", "arena_installed_service_reconciliation_v1"},
          {"state", service_state}}},
        {"installed_engine_reconciliation",
         {{"schema", "arena_installed_engine_reconciliation_v1"},
          {"state", engine_state}}},
    };
}

}  // namespace

TEST_CASE("cold Manager readiness may arrive after ten seconds") {
    std::chrono::steady_clock::time_point current{};
    const auto result = arena::application::wait_for_manager_startup(
        [&] {
            return ManagerStartupProbe{
                .state = current.time_since_epoch() >= 12s
                    ? ManagerStartupProbeState::ready
                    : ManagerStartupProbeState::pending,
                .detail = "core startup in progress",
            };
        },
        [] { return std::optional<std::uint32_t>{}; },
        [&] { return current; },
        [&](const std::chrono::milliseconds duration) { current += duration; },
        120s,
        1s);

    REQUIRE(result.state == ManagerStartupWaitState::ready);
    REQUIRE(result.elapsed == 12s);
    REQUIRE(result.observations == 13);
}

TEST_CASE("a live Manager remains starting until the lifecycle deadline") {
    std::chrono::steady_clock::time_point current{};
    std::size_t exit_observations = 0;
    const auto result = arena::application::wait_for_manager_startup(
        [] {
            return ManagerStartupProbe{
                .state = ManagerStartupProbeState::pending,
                .detail = "cold core initialization",
            };
        },
        [&] {
            ++exit_observations;
            return std::optional<std::uint32_t>{};
        },
        [&] { return current; },
        [&](const std::chrono::milliseconds duration) { current += duration; },
        15s,
        1s);

    REQUIRE(result.state == ManagerStartupWaitState::timed_out);
    REQUIRE(result.elapsed == 15s);
    REQUIRE(exit_observations == 16);
}

TEST_CASE("Manager child exit is distinct from readiness failure") {
    std::chrono::steady_clock::time_point current{};
    const auto result = arena::application::wait_for_manager_startup(
        [] {
            return ManagerStartupProbe{
                .state = ManagerStartupProbeState::pending,
                .detail = "core startup in progress",
            };
        },
        [&] {
            return current.time_since_epoch() >= 3s
                ? std::optional<std::uint32_t>{27}
                : std::optional<std::uint32_t>{};
        },
        [&] { return current; },
        [&](const std::chrono::milliseconds duration) { current += duration; },
        120s,
        1s);

    REQUIRE(result.state == ManagerStartupWaitState::service_exited);
    REQUIRE(result.service_exit_code == 27);
    REQUIRE(result.elapsed == 3s);
}

TEST_CASE("authenticated core readiness does not wait for reconciliation") {
    REQUIRE(arena::application::manager_readiness_error(
                readiness_document(), "test-version")
                .empty());
    REQUIRE(arena::application::manager_readiness_error(
                readiness_document("running", "complete_with_errors"),
                "test-version")
                .empty());
}

TEST_CASE("an incompatible listener cannot satisfy Manager readiness") {
    auto wrong_schema = readiness_document();
    wrong_schema["schema"] = "foreign_service_readiness_v1";
    REQUIRE_FALSE(arena::application::manager_readiness_error(
                      wrong_schema, "test-version")
                      .empty());

    auto wrong_service = readiness_document();
    wrong_service["service"] = "foreign-service";
    REQUIRE_FALSE(arena::application::manager_readiness_error(
                      wrong_service, "test-version")
                      .empty());

    auto wrong_version = readiness_document();
    REQUIRE_FALSE(arena::application::manager_readiness_error(
                      wrong_version, "other-version")
                      .empty());
}

TEST_CASE("desktop startup diagnostics are stable distinct categories") {
    constexpr std::array categories{
        DesktopStartupFailureCategory::service_launch,
        DesktopStartupFailureCategory::service_process_exit,
        DesktopStartupFailureCategory::service_readiness,
        DesktopStartupFailureCategory::pair_helper,
        DesktopStartupFailureCategory::webview_initialization,
    };
    std::set<std::string> names;
    for (const auto category : categories) {
        names.emplace(arena::application::desktop_startup_category(category));
    }
    REQUIRE(names.size() == categories.size());
}

TEST_CASE("desktop startup detail is bounded and redacts bearer and pairing secrets") {
    const std::string detail =
        "request\r\nBearer super-secret #pair=pair-secret&picker_token=picker-secret " +
        std::string(800, 'x');
    const std::string sanitized =
        arena::application::bounded_startup_detail(detail);

    REQUIRE(sanitized.size() <= 512);
    REQUIRE(sanitized.find('\n') == std::string::npos);
    REQUIRE(sanitized.find("super-secret") == std::string::npos);
    REQUIRE(sanitized.find("pair-secret") == std::string::npos);
    REQUIRE(sanitized.find("picker-secret") == std::string::npos);
    REQUIRE(sanitized.find("<redacted>") != std::string::npos);
}

TEST_CASE("second desktop launch activates the existing authority") {
    REQUIRE(
        arena::application::desktop_launch_disposition(false) ==
        DesktopLaunchDisposition::start_authority);
    REQUIRE(
        arena::application::desktop_launch_disposition(true) ==
        DesktopLaunchDisposition::activate_existing);
}
