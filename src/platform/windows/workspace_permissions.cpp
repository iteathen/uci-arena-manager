#include "arena/platform/workspace_permissions.hpp"

#include <aclapi.h>
#include <windows.h>

#include <array>
#include <stdexcept>
#include <string>

namespace arena::platform {
namespace {

[[noreturn]] void throw_windows_error(const std::string& operation, const DWORD error) {
    throw std::runtime_error(operation + " failed with Windows error " +
                             std::to_string(error));
}

} // namespace

void grant_manager_workspace_access(
    const std::filesystem::path& workspace,
    const bool manager_service_access) {
    if (!std::filesystem::is_directory(workspace) ||
        std::filesystem::is_symlink(workspace)) {
        throw std::runtime_error(
            "managed service workspace is unavailable for permission setup");
    }

    if (!manager_service_access) {
        return;
    }

    std::array<unsigned char, SECURITY_MAX_SID_SIZE> sid_buffer{};
    DWORD sid_size = static_cast<DWORD>(sid_buffer.size());
    if (!CreateWellKnownSid(WinLocalServiceSid, nullptr, sid_buffer.data(), &sid_size)) {
        throw_windows_error("CreateWellKnownSid", GetLastError());
    }

    PSECURITY_DESCRIPTOR security_descriptor = nullptr;
    PACL current_dacl = nullptr;
    std::wstring path = workspace.native();
    const DWORD read_result = GetNamedSecurityInfoW(
        path.data(), SE_FILE_OBJECT, DACL_SECURITY_INFORMATION, nullptr, nullptr,
        &current_dacl, nullptr, &security_descriptor);
    if (read_result != ERROR_SUCCESS) {
        throw_windows_error("GetNamedSecurityInfoW", read_result);
    }

    EXPLICIT_ACCESSW access{};
    access.grfAccessPermissions =
        FILE_GENERIC_READ | FILE_GENERIC_WRITE | FILE_GENERIC_EXECUTE | DELETE;
    access.grfAccessMode = GRANT_ACCESS;
    access.grfInheritance = SUB_CONTAINERS_AND_OBJECTS_INHERIT;
    access.Trustee.TrusteeForm = TRUSTEE_IS_SID;
    access.Trustee.TrusteeType = TRUSTEE_IS_USER;
    access.Trustee.ptstrName =
        reinterpret_cast<LPWSTR>(sid_buffer.data());

    PACL updated_dacl = nullptr;
    const DWORD merge_result =
        SetEntriesInAclW(1, &access, current_dacl, &updated_dacl);
    if (merge_result != ERROR_SUCCESS) {
        LocalFree(security_descriptor);
        throw_windows_error("SetEntriesInAclW", merge_result);
    }

    const DWORD write_result = SetNamedSecurityInfoW(
        path.data(), SE_FILE_OBJECT, DACL_SECURITY_INFORMATION, nullptr, nullptr,
        updated_dacl, nullptr);
    LocalFree(updated_dacl);
    LocalFree(security_descriptor);
    if (write_result != ERROR_SUCCESS) {
        throw_windows_error("SetNamedSecurityInfoW", write_result);
    }
}

} // namespace arena::platform
