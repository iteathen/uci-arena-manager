#include "arena/application/broker_policy.hpp"

#define NOMINMAX
#include <windows.h>
#include <winsvc.h>

#include <algorithm>
#include <stdexcept>
#include <string>

namespace arena::application {
namespace {

std::wstring wide(const std::string& value) {
    const int count = MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), nullptr, 0);
    if (count <= 0) {
        throw std::runtime_error("service name is not valid UTF-8");
    }
    std::wstring result(static_cast<std::size_t>(count), L'\0');
    MultiByteToWideChar(
        CP_UTF8,
        MB_ERR_INVALID_CHARS,
        value.data(),
        static_cast<int>(value.size()),
        result.data(),
        count);
    return result;
}

constexpr DWORD transition_timeout_ms = 30'000;

SERVICE_STATUS_PROCESS query_service_status(const SC_HANDLE service) {
    SERVICE_STATUS_PROCESS status{};
    DWORD required = 0;
    if (!QueryServiceStatusEx(
            service,
            SC_STATUS_PROCESS_INFO,
            reinterpret_cast<LPBYTE>(&status),
            sizeof(status),
            &required)) {
        throw std::runtime_error("Windows service status query failed with error " +
                                 std::to_string(GetLastError()));
    }
    return status;
}

void wait_for_service_state(const SC_HANDLE service, const DWORD expected_state,
                            const std::string& action) {
    const ULONGLONG deadline = GetTickCount64() + transition_timeout_ms;
    SERVICE_STATUS_PROCESS status = query_service_status(service);
    while (status.dwCurrentState != expected_state) {
        if (GetTickCount64() >= deadline) {
            throw std::runtime_error(
                "Windows service " + action + " timed out in SCM state " +
                std::to_string(status.dwCurrentState));
        }
        const DWORD delay = std::clamp(status.dwWaitHint / 10, 50UL, 250UL);
        Sleep(delay);
        status = query_service_status(service);
    }
}

SERVICE_STATUS_PROCESS wait_for_start_resolution(const SC_HANDLE service) {
    const ULONGLONG deadline = GetTickCount64() + transition_timeout_ms;
    SERVICE_STATUS_PROCESS status = query_service_status(service);
    while (status.dwCurrentState == SERVICE_START_PENDING) {
        if (GetTickCount64() >= deadline) {
            throw std::runtime_error("Windows service start timed out in SCM state " +
                                     std::to_string(status.dwCurrentState));
        }
        const DWORD delay = std::clamp(status.dwWaitHint / 10, 50UL, 250UL);
        Sleep(delay);
        status = query_service_status(service);
    }
    return status;
}

void start_service(const SC_HANDLE service) {
    SERVICE_STATUS_PROCESS status = query_service_status(service);
    if (status.dwCurrentState == SERVICE_RUNNING) {
        return;
    }
    if (status.dwCurrentState == SERVICE_START_PENDING) {
        status = wait_for_start_resolution(service);
        if (status.dwCurrentState == SERVICE_RUNNING) {
            return;
        }
    }
    if (status.dwCurrentState == SERVICE_STOP_PENDING) {
        wait_for_service_state(service, SERVICE_STOPPED, "stop");
        status = query_service_status(service);
    }
    if (status.dwCurrentState != SERVICE_STOPPED) {
        throw std::runtime_error("Windows service cannot start from SCM state " +
                                 std::to_string(status.dwCurrentState));
    }
    if (!StartServiceW(service, 0, nullptr)) {
        throw std::runtime_error("Windows service start failed with error " +
                                 std::to_string(GetLastError()));
    }
    wait_for_service_state(service, SERVICE_RUNNING, "start");
}

void stop_service(const SC_HANDLE service) {
    SERVICE_STATUS_PROCESS status = wait_for_start_resolution(service);
    if (status.dwCurrentState == SERVICE_STOPPED) {
        return;
    }
    if (status.dwCurrentState == SERVICE_STOP_PENDING) {
        wait_for_service_state(service, SERVICE_STOPPED, "stop");
        return;
    }
    SERVICE_STATUS control_status{};
    if (!ControlService(service, SERVICE_CONTROL_STOP, &control_status) &&
        GetLastError() != ERROR_SERVICE_NOT_ACTIVE) {
        throw std::runtime_error("Windows service stop failed with error " +
                                 std::to_string(GetLastError()));
    }
    wait_for_service_state(service, SERVICE_STOPPED, "stop");
}

}  // namespace

int perform_service_action(const std::string& platform_name, const std::string& action) {
    SC_HANDLE manager = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (manager == nullptr) {
        throw std::runtime_error("cannot open Windows Service Control Manager");
    }
    const DWORD access =
        SERVICE_QUERY_STATUS |
        (action == "start" || action == "restart" ? SERVICE_START : 0) |
        (action == "stop" || action == "restart" ? SERVICE_STOP : 0);
    const std::wstring service_name = wide(platform_name);
    SC_HANDLE service = OpenServiceW(manager, service_name.c_str(), access);
    if (service == nullptr) {
        CloseServiceHandle(manager);
        throw std::runtime_error("allowlisted Windows service cannot be opened");
    }

    try {
        if (action == "start") {
            start_service(service);
        } else if (action == "stop") {
            stop_service(service);
        } else if (action == "restart") {
            stop_service(service);
            start_service(service);
        } else if (action == "status") {
            const int state = static_cast<int>(query_service_status(service).dwCurrentState);
            CloseServiceHandle(service);
            CloseServiceHandle(manager);
            return state;
        } else {
            throw std::runtime_error("unsupported Windows service action: " + action);
        }
        CloseServiceHandle(service);
        CloseServiceHandle(manager);
        return 0;
    } catch (...) {
        CloseServiceHandle(service);
        CloseServiceHandle(manager);
        throw;
    }
}

}  // namespace arena::application
