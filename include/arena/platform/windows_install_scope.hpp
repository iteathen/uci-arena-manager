#pragma once

#ifdef _WIN32

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <filesystem>
#include <fstream>
#include <string>

namespace arena::platform {

enum class WindowsInstallScope {
    per_user,
    per_machine,
};

inline std::filesystem::path windows_executable_path() {
    std::wstring buffer(32768, L'\0');
    const DWORD length = GetModuleFileNameW(
        nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    if (length == 0 || length >= buffer.size()) {
        return {};
    }
    buffer.resize(length);
    return std::filesystem::path(buffer);
}

inline WindowsInstallScope windows_install_scope() {
    const std::filesystem::path executable = windows_executable_path();
    if (executable.empty()) {
        return WindowsInstallScope::per_machine;
    }
    const std::filesystem::path marker =
        executable.parent_path().parent_path() /
        L"share" / L"uci-arena" / L"install-scope.txt";
    std::ifstream input(marker, std::ios::binary);
    std::string value;
    std::getline(input, value);
    if (!value.empty() && value.back() == '\r') {
        value.pop_back();
    }
    if (value == "per-user") {
        return WindowsInstallScope::per_user;
    }
    return WindowsInstallScope::per_machine;
}

inline bool is_windows_per_user_install() {
    return windows_install_scope() == WindowsInstallScope::per_user;
}

} // namespace arena::platform

#endif
