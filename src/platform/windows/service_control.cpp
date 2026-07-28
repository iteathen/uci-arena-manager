#include "arena/application/broker_policy.hpp"

#define NOMINMAX
#include <windows.h>
#include <winsvc.h>

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

    SERVICE_STATUS status{};
    BOOL ok = TRUE;
    if (action == "start") {
        ok = StartServiceW(service, 0, nullptr);
        if (!ok && GetLastError() == ERROR_SERVICE_ALREADY_RUNNING) {
            ok = TRUE;
        }
    } else if (action == "stop") {
        ok = ControlService(service, SERVICE_CONTROL_STOP, &status);
        if (!ok && GetLastError() == ERROR_SERVICE_NOT_ACTIVE) {
            ok = TRUE;
        }
    } else if (action == "restart") {
        ok = ControlService(service, SERVICE_CONTROL_STOP, &status);
        if (!ok && GetLastError() != ERROR_SERVICE_NOT_ACTIVE) {
            CloseServiceHandle(service);
            CloseServiceHandle(manager);
            throw std::runtime_error("Windows service stop failed");
        }
        for (int attempt = 0; attempt < 100; ++attempt) {
            SERVICE_STATUS_PROCESS current{};
            DWORD required = 0;
            QueryServiceStatusEx(
                service,
                SC_STATUS_PROCESS_INFO,
                reinterpret_cast<LPBYTE>(&current),
                sizeof(current),
                &required);
            if (current.dwCurrentState == SERVICE_STOPPED) {
                break;
            }
            Sleep(50);
        }
        ok = StartServiceW(service, 0, nullptr);
    } else {
        SERVICE_STATUS_PROCESS current{};
        DWORD required = 0;
        ok = QueryServiceStatusEx(
            service,
            SC_STATUS_PROCESS_INFO,
            reinterpret_cast<LPBYTE>(&current),
            sizeof(current),
            &required);
        if (ok) {
            CloseServiceHandle(service);
            CloseServiceHandle(manager);
            return static_cast<int>(current.dwCurrentState);
        }
    }
    const DWORD error = ok ? ERROR_SUCCESS : GetLastError();
    CloseServiceHandle(service);
    CloseServiceHandle(manager);
    if (!ok) {
        throw std::runtime_error("Windows service action failed with error " +
                                 std::to_string(error));
    }
    return 0;
}

}  // namespace arena::application
