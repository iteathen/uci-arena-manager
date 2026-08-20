#include "arena/application/manager_lifecycle.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <stdexcept>

#include <nlohmann/json.hpp>

namespace arena::application {
namespace {

bool valid_reconciliation(
    const nlohmann::json& value,
    const std::string_view expected_schema) {
    static constexpr std::array states{
        "not_run",
        "pending",
        "running",
        "complete",
        "complete_with_errors",
        "failed",
    };
    if (!value.is_object() ||
        value.value("schema", "") != expected_schema ||
        !value.contains("state") || !value.at("state").is_string()) {
        return false;
    }
    const auto& state = value.at("state").get_ref<const std::string&>();
    return std::ranges::find(states, state) != states.end();
}

void redact_value(std::string& value, const std::string_view marker) {
    std::size_t cursor = 0;
    while ((cursor = value.find(marker, cursor)) != std::string::npos) {
        const std::size_t begin = cursor + marker.size();
        std::size_t end = begin;
        while (end < value.size() &&
               !std::isspace(static_cast<unsigned char>(value[end])) &&
               value[end] != '&') {
            ++end;
        }
        value.replace(begin, end - begin, "<redacted>");
        cursor = begin + std::string_view{"<redacted>"}.size();
    }
}

}  // namespace

ManagerStartupWaitResult wait_for_manager_startup(
    const ManagerStartupProbeFunction& readiness_probe,
    const ManagerServiceExitProbe& service_exit_probe,
    const ManagerStartupNow& now,
    const ManagerStartupSleep& sleep,
    const std::chrono::milliseconds deadline,
    const std::chrono::milliseconds poll_interval) {
    if (!readiness_probe || !service_exit_probe || !now || !sleep ||
        deadline <= std::chrono::milliseconds::zero() ||
        poll_interval <= std::chrono::milliseconds::zero()) {
        throw std::invalid_argument("Manager startup wait contract is invalid");
    }
    const auto started = now();
    const auto expires = started + deadline;
    std::size_t observations = 0;
    std::string last_detail;
    while (true) {
        if (const auto exit_code = service_exit_probe(); exit_code.has_value()) {
            return {
                .state = ManagerStartupWaitState::service_exited,
                .detail = "the Manager service process exited during core startup",
                .service_exit_code = exit_code,
                .observations = observations,
                .elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                    now() - started),
            };
        }
        const ManagerStartupProbe observation = readiness_probe();
        ++observations;
        if (!observation.detail.empty()) {
            last_detail = bounded_startup_detail(observation.detail);
        }
        if (observation.state == ManagerStartupProbeState::ready) {
            return {
                .state = ManagerStartupWaitState::ready,
                .detail = std::move(last_detail),
                .observations = observations,
                .elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                    now() - started),
            };
        }
        if (observation.state == ManagerStartupProbeState::failed) {
            return {
                .state = ManagerStartupWaitState::probe_failed,
                .detail = last_detail.empty()
                    ? "Manager authenticated readiness failed"
                    : std::move(last_detail),
                .observations = observations,
                .elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                    now() - started),
            };
        }
        const auto current = now();
        if (current >= expires) {
            return {
                .state = ManagerStartupWaitState::timed_out,
                .detail = last_detail.empty()
                    ? "Manager core startup remained in progress until its lifecycle deadline"
                    : std::move(last_detail),
                .observations = observations,
                .elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                    current - started),
            };
        }
        sleep(std::min(
            poll_interval,
            std::chrono::duration_cast<std::chrono::milliseconds>(
                expires - current)));
    }
}

std::string_view desktop_startup_category(
    const DesktopStartupFailureCategory category) noexcept {
    switch (category) {
        case DesktopStartupFailureCategory::service_launch:
            return "service_launch";
        case DesktopStartupFailureCategory::service_process_exit:
            return "service_process_exit";
        case DesktopStartupFailureCategory::service_readiness:
            return "service_readiness";
        case DesktopStartupFailureCategory::pair_helper:
            return "pair_helper";
        case DesktopStartupFailureCategory::webview_initialization:
            return "webview_initialization";
    }
    return "service_readiness";
}

std::string bounded_startup_detail(const std::string_view detail) {
    std::string normalized;
    normalized.reserve(std::min<std::size_t>(detail.size(), 512));
    bool previous_space = false;
    for (const unsigned char character : detail) {
        const bool space = std::isspace(character) || std::iscntrl(character);
        if (space) {
            if (!normalized.empty() && !previous_space) normalized.push_back(' ');
            previous_space = true;
        } else {
            normalized.push_back(static_cast<char>(character));
            previous_space = false;
        }
        if (normalized.size() >= 2048) break;
    }
    while (!normalized.empty() && normalized.back() == ' ') normalized.pop_back();
    redact_value(normalized, "Bearer ");
    redact_value(normalized, "#pair=");
    redact_value(normalized, "picker_token=");
    if (normalized.size() > 512) normalized.resize(512);
    return normalized.empty() ? "startup failed without additional detail" : normalized;
}

std::string manager_readiness_error(
    const nlohmann::json& document,
    const std::string_view expected_version) {
    if (!document.is_object() ||
        document.value("schema", "") != manager_readiness_schema ||
        document.value("service", "") != "uci-arena-manager" ||
        document.value("version", "") != expected_version ||
        document.value("state", "") != "core_ready" ||
        !document.contains("started_at") ||
        !document.at("started_at").is_string() ||
        document.at("started_at").get_ref<const std::string&>().empty()) {
        return "the authenticated endpoint returned an incompatible Manager readiness document";
    }
    if (!document.contains("installed_service_reconciliation") ||
        !valid_reconciliation(
            document.at("installed_service_reconciliation"),
            "arena_installed_service_reconciliation_v1") ||
        !document.contains("installed_engine_reconciliation") ||
        !valid_reconciliation(
            document.at("installed_engine_reconciliation"),
            "arena_installed_engine_reconciliation_v1")) {
        return "the authenticated endpoint returned invalid startup reconciliation state";
    }
    return {};
}

}  // namespace arena::application
