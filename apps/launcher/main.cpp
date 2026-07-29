#include <filesystem>
#include <string>

#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#include <shellapi.h>

namespace {

std::wstring quote(const std::wstring& value) {
    return L"\"" + value + L"\"";
}

int show_error(const wchar_t* detail) {
    MessageBoxW(nullptr, detail, L"UCI Arena Manager", MB_OK | MB_ICONERROR);
    return 1;
}

}  // namespace

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    HANDLE mutex = CreateMutexW(
        nullptr, FALSE, L"Local\\UCIArenaManagerInteractiveLauncher");
    if (mutex == nullptr) {
        return show_error(L"Cannot initialize the interactive Manager session.");
    }
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        ShellExecuteW(
            nullptr, L"open", L"http://127.0.0.1:47831/", nullptr, nullptr,
            SW_SHOWNORMAL);
        CloseHandle(mutex);
        return 0;
    }

    wchar_t module[MAX_PATH]{};
    const DWORD length = GetModuleFileNameW(nullptr, module, MAX_PATH);
    if (length == 0 || length == MAX_PATH) {
        CloseHandle(mutex);
        return show_error(L"Cannot resolve the installed Manager directory.");
    }
    const std::filesystem::path cli =
        std::filesystem::path(module).parent_path() / L"arena.exe";
    std::wstring command = quote(cli.wstring()) + L" ui";
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(
            cli.c_str(), command.data(), nullptr, nullptr, FALSE,
            CREATE_NO_WINDOW, nullptr, cli.parent_path().c_str(), &startup,
            &process)) {
        CloseHandle(mutex);
        return show_error(
            L"Cannot start arena.exe. Repair UCI Arena Manager and try again.");
    }
    CloseHandle(process.hThread);
    WaitForSingleObject(process.hProcess, INFINITE);
    DWORD exit_code = 1;
    GetExitCodeProcess(process.hProcess, &exit_code);
    CloseHandle(process.hProcess);
    CloseHandle(mutex);
    if (exit_code != 0) {
        return show_error(
            L"The local control plane is unavailable. Open Services and start "
            L"UCI Arena Manager, or run arena doctor for details.");
    }
    return 0;
}
#else
#include <unistd.h>

int main() {
    const std::filesystem::path cli =
        std::filesystem::canonical("/proc/self/exe").parent_path() / "arena";
    execl(cli.c_str(), cli.c_str(), "ui", static_cast<char*>(nullptr));
    return 1;
}
#endif
