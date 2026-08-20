#include "webview/webview.h"

#include "arena/application/manager_lifecycle.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>

#ifdef _WIN32
#include "arena/platform/windows_install_scope.hpp"
#define NOMINMAX
#include <winsock2.h>
#include <shlobj.h>
#include <windows.h>

#include <nlohmann/json.hpp>

namespace {

constexpr wchar_t kWindowTitle[] = L"UCI Arena Manager";
constexpr wchar_t kLauncherMutex[] =
    L"Local\\UCIArenaManagerInteractiveLauncher";

class DesktopStartupError final : public std::runtime_error {
public:
    DesktopStartupError(
        const arena::application::DesktopStartupFailureCategory category,
        std::string detail)
        : std::runtime_error(arena::application::bounded_startup_detail(detail)),
          category_(category) {}

    [[nodiscard]] arena::application::DesktopStartupFailureCategory category()
        const noexcept {
        return category_;
    }

private:
    arena::application::DesktopStartupFailureCategory category_;
};

std::wstring quote(const std::wstring& value) {
    return L"\"" + value + L"\"";
}

int show_error(const std::wstring& detail) {
    MessageBoxW(nullptr, detail.c_str(), kWindowTitle, MB_OK | MB_ICONERROR);
    return 1;
}

std::wstring widen(const std::string_view value) {
    if (value.empty()) return {};
    const int length = MultiByteToWideChar(
        CP_UTF8,
        MB_ERR_INVALID_CHARS,
        value.data(),
        static_cast<int>(value.size()),
        nullptr,
        0);
    if (length <= 0) return L"startup detail was not valid UTF-8";
    std::wstring result(static_cast<std::size_t>(length), L'\0');
    MultiByteToWideChar(
        CP_UTF8,
        MB_ERR_INVALID_CHARS,
        value.data(),
        static_cast<int>(value.size()),
        result.data(),
        length);
    return result;
}

int show_startup_error(const DesktopStartupError& error) {
    const std::string_view category =
        arena::application::desktop_startup_category(error.category());
    std::wstring message =
        L"UCI Arena Manager could not open.\n\nCategory: " + widen(category) +
        L"\nDetail: " + widen(error.what()) + L"\n\n";
    switch (error.category()) {
        case arena::application::DesktopStartupFailureCategory::service_launch:
        case arena::application::DesktopStartupFailureCategory::service_process_exit:
        case arena::application::DesktopStartupFailureCategory::service_readiness:
            message += L"Review the Manager service installation and local service logs.";
            break;
        case arena::application::DesktopStartupFailureCategory::pair_helper:
            message += L"Repair the installed Manager CLI and retry.";
            break;
        case arena::application::DesktopStartupFailureCategory::webview_initialization:
            message += L"Verify Microsoft Edge WebView2 Runtime, then repair UCI Arena Suite Setup.";
            break;
    }
    return show_error(message);
}

void activate_existing_window() {
    if (HWND window = FindWindowW(nullptr, kWindowTitle); window != nullptr) {
        if (IsIconic(window)) {
            ShowWindow(window, SW_RESTORE);
        }
        ShowWindow(window, SW_SHOW);
        SetForegroundWindow(window);
    }
}

std::filesystem::path executable_directory() {
    std::wstring buffer(32768, L'\0');
    const DWORD length = GetModuleFileNameW(
        nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    if (length == 0 || length >= buffer.size()) {
        throw std::runtime_error("cannot resolve the installed Manager directory");
    }
    buffer.resize(length);
    return std::filesystem::path(buffer).parent_path();
}

void configure_webview_user_data() {
    PWSTR raw_path = nullptr;
    const HRESULT result = SHGetKnownFolderPath(
        FOLDERID_LocalAppData, KF_FLAG_CREATE, nullptr, &raw_path);
    if (FAILED(result) || raw_path == nullptr) {
        throw std::runtime_error("cannot resolve the interactive user data directory");
    }
    const std::filesystem::path root =
        std::filesystem::path(raw_path) / L"UCI Arena Manager" / L"WebView2";
    CoTaskMemFree(raw_path);
    std::filesystem::create_directories(root);
    if (!SetEnvironmentVariableW(
            L"WEBVIEW2_USER_DATA_FOLDER", root.c_str())) {
        throw std::runtime_error("cannot configure the embedded browser profile");
    }
}

std::uint16_t find_available_picker_port() {
    WSADATA winsock{};
    if (WSAStartup(MAKEWORD(2, 2), &winsock) != 0) {
        throw std::runtime_error(
            "cannot initialize the native helper network");
    }
    const SOCKET listener =
        socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listener == INVALID_SOCKET) {
        WSACleanup();
        throw std::runtime_error(
            "cannot allocate the native helper endpoint");
    }
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = 0;
    int address_size = sizeof(address);
    const bool bound =
        bind(
            listener,
            reinterpret_cast<const sockaddr*>(&address),
            sizeof(address)) == 0;
    const bool resolved =
        bound &&
        getsockname(
            listener,
            reinterpret_cast<sockaddr*>(&address),
            &address_size) == 0;
    closesocket(listener);
    WSACleanup();
    const std::uint16_t port = ntohs(address.sin_port);
    if (!resolved || port < 1024) {
        throw std::runtime_error(
            "cannot select the native helper endpoint");
    }
    return port;
}

bool manager_port_connectable() {
    WSADATA winsock{};
    if (WSAStartup(MAKEWORD(2, 2), &winsock) != 0) {
        return false;
    }
    const SOCKET connection = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (connection == INVALID_SOCKET) {
        WSACleanup();
        return false;
    }
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = htons(47831);
    const bool available = connect(
        connection,
        reinterpret_cast<const sockaddr*>(&address),
        sizeof(address)) == 0;
    closesocket(connection);
    WSACleanup();
    return available;
}

std::filesystem::path per_user_data_directory() {
    PWSTR raw_path = nullptr;
    const HRESULT result = SHGetKnownFolderPath(
        FOLDERID_LocalAppData, KF_FLAG_CREATE, nullptr, &raw_path);
    if (FAILED(result) || raw_path == nullptr) {
        throw std::runtime_error(
            "cannot resolve the per-user Manager data directory");
    }
    const std::filesystem::path path =
        std::filesystem::path(raw_path) / L"UCI Arena Manager";
    CoTaskMemFree(raw_path);
    return path;
}

class PerUserManagerProcess final {
public:
    explicit PerUserManagerProcess(
        const std::filesystem::path& executable_root) {
        if (manager_port_connectable()) {
            return;
        }
        const std::filesystem::path executable =
            executable_root / L"arena-service.exe";
        const std::filesystem::path web_root =
            executable_root.parent_path() / L"share" / L"uci-arena" / L"web";
        if (!std::filesystem::is_regular_file(executable) ||
            !std::filesystem::is_directory(web_root)) {
            throw std::runtime_error(
                "the installed per-user Manager runtime is incomplete");
        }

        STARTUPINFOW startup{};
        startup.cb = sizeof(startup);
        PROCESS_INFORMATION process{};
        std::wstring command =
            quote(executable.wstring()) +
            L" --data-dir " + quote(per_user_data_directory().wstring()) +
            L" --web-root " + quote(web_root.wstring());
        const BOOL created = CreateProcessW(
            executable.c_str(),
            command.data(),
            nullptr,
            nullptr,
            FALSE,
            CREATE_NO_WINDOW | CREATE_NEW_PROCESS_GROUP,
            nullptr,
            executable_root.c_str(),
            &startup,
            &process);
        if (!created) {
            throw std::runtime_error(
                "cannot start the per-user Manager process");
        }
        process_ = process.hProcess;
        CloseHandle(process.hThread);

        job_ = CreateJobObjectW(nullptr, nullptr);
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
        limits.BasicLimitInformation.LimitFlags =
            JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        if (job_ == nullptr ||
            !SetInformationJobObject(
                job_, JobObjectExtendedLimitInformation, &limits,
                sizeof(limits)) ||
            !AssignProcessToJobObject(job_, process_)) {
            TerminateProcess(process_, 1);
            WaitForSingleObject(process_, 3000);
            close_handles();
            throw std::runtime_error(
                "cannot contain the per-user Manager process");
        }

    }

    PerUserManagerProcess(const PerUserManagerProcess&) = delete;
    PerUserManagerProcess& operator=(const PerUserManagerProcess&) = delete;

    ~PerUserManagerProcess() {
        if (job_ != nullptr) {
            CloseHandle(job_);
            job_ = nullptr;
        }
        if (process_ != nullptr) {
            WaitForSingleObject(process_, 3000);
            CloseHandle(process_);
            process_ = nullptr;
        }
    }

    [[nodiscard]] std::optional<std::uint32_t> exit_code() const {
        if (process_ == nullptr ||
            WaitForSingleObject(process_, 0) != WAIT_OBJECT_0) {
            return std::nullopt;
        }
        DWORD code = 0;
        return GetExitCodeProcess(process_, &code) ?
            std::optional<std::uint32_t>{code} :
            std::optional<std::uint32_t>{1};
    }

private:
    void close_handles() {
        if (job_ != nullptr) {
            CloseHandle(job_);
            job_ = nullptr;
        }
        if (process_ != nullptr) {
            CloseHandle(process_);
            process_ = nullptr;
        }
    }

    HANDLE process_{};
    HANDLE job_{};
};

class UiHelper final {
public:
    UiHelper(
        const std::filesystem::path& cli,
        const std::uint16_t picker_port) {
        SECURITY_ATTRIBUTES attributes{};
        attributes.nLength = sizeof(attributes);
        attributes.bInheritHandle = TRUE;

        HANDLE output_write = nullptr;
        if (!CreatePipe(&output_read_, &output_write, &attributes, 0) ||
            !SetHandleInformation(output_read_, HANDLE_FLAG_INHERIT, 0)) {
            close_handle(output_write);
            throw std::runtime_error("cannot create the desktop helper channel");
        }

        HANDLE null_input = CreateFileW(
            L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &attributes,
            OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        HANDLE null_error = CreateFileW(
            L"NUL", GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &attributes,
            OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (null_input == INVALID_HANDLE_VALUE ||
            null_error == INVALID_HANDLE_VALUE) {
            close_handle(output_write);
            close_handle(null_input);
            close_handle(null_error);
            throw std::runtime_error("cannot initialize the desktop helper handles");
        }

        STARTUPINFOW startup{};
        startup.cb = sizeof(startup);
        startup.dwFlags = STARTF_USESTDHANDLES;
        startup.hStdInput = null_input;
        startup.hStdOutput = output_write;
        startup.hStdError = null_error;
        PROCESS_INFORMATION process{};
        std::wstring command =
            quote(cli.wstring()) +
            L" ui --no-open --desktop-host --picker-port " +
            std::to_wstring(picker_port) +
            L" --startup-wait-ms " +
            std::to_wstring(
                arena::application::manager_core_readiness_deadline.count());
        const BOOL created = CreateProcessW(
            cli.c_str(), command.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW,
            nullptr, cli.parent_path().c_str(), &startup, &process);
        close_handle(output_write);
        close_handle(null_input);
        close_handle(null_error);
        if (!created) {
            close_handle(output_read_);
            throw std::runtime_error(
                "cannot start the authenticated desktop helper");
        }
        process_ = process.hProcess;
        scratch_path_ =
            std::filesystem::temp_directory_path() /
            ("uci-arena-manager-ui-" +
             std::to_string(process.dwProcessId));

        job_ = CreateJobObjectW(nullptr, nullptr);
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
        limits.BasicLimitInformation.LimitFlags =
            JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        if (job_ == nullptr ||
            !SetInformationJobObject(
                job_, JobObjectExtendedLimitInformation, &limits,
                sizeof(limits)) ||
            !AssignProcessToJobObject(job_, process_)) {
            TerminateProcess(process_, 1);
            WaitForSingleObject(process_, 3000);
            remove_scratch();
            CloseHandle(process.hThread);
            close_handle(job_);
            close_handle(process_);
            close_handle(output_read_);
            throw std::runtime_error(
                "cannot contain the authenticated desktop helper");
        }
        CloseHandle(process.hThread);
    }

    UiHelper(const UiHelper&) = delete;
    UiHelper& operator=(const UiHelper&) = delete;

    ~UiHelper() {
        close_handle(job_);
        if (process_ != nullptr) {
            WaitForSingleObject(process_, 3000);
        }
        remove_scratch();
        close_handle(process_);
        close_handle(output_read_);
    }

    std::string pairing_url(const PerUserManagerProcess* manager) {
        std::string output;
        std::string url;
        const auto result = arena::application::wait_for_manager_startup(
            [&]() {
                DWORD available = 0;
                if (!PeekNamedPipe(
                        output_read_, nullptr, 0, nullptr, &available, nullptr)) {
                    if (WaitForSingleObject(process_, 0) == WAIT_OBJECT_0) {
                        throw DesktopStartupError(
                            arena::application::DesktopStartupFailureCategory::pair_helper,
                            "the authenticated desktop helper exited without a result");
                    }
                    throw DesktopStartupError(
                        arena::application::DesktopStartupFailureCategory::pair_helper,
                        "the authenticated desktop helper channel failed");
                }
                if (available > 0) {
                    char buffer[1024];
                    DWORD read = 0;
                    if (!ReadFile(
                            output_read_, buffer,
                            static_cast<DWORD>(std::min<std::size_t>(
                                sizeof(buffer), available)),
                            &read, nullptr)) {
                        throw DesktopStartupError(
                            arena::application::DesktopStartupFailureCategory::pair_helper,
                            "the authenticated desktop helper did not respond");
                    }
                    output.append(buffer, read);
                    if (output.size() > 4096) {
                        throw DesktopStartupError(
                            arena::application::DesktopStartupFailureCategory::pair_helper,
                            "the authenticated desktop helper response was too large");
                    }
                    if (const auto newline = output.find('\n');
                        newline != std::string::npos) {
                        std::string line = output.substr(0, newline);
                        if (!line.empty() && line.back() == '\r') line.pop_back();
                        if (line.starts_with(
                                "http://127.0.0.1:47831/#pair=") &&
                            line.size() <= 4096) {
                            url = std::move(line);
                            return arena::application::ManagerStartupProbe{
                                .state = arena::application::ManagerStartupProbeState::ready,
                                .detail = "authenticated desktop pairing completed",
                            };
                        }
                        try {
                            const auto failure = nlohmann::json::parse(line);
                            if (failure.value("schema", "") ==
                                    "arena_desktop_helper_result_v1" &&
                                failure.value("state", "") == "failed") {
                                const std::string category =
                                    failure.value("category", "pair_helper");
                                throw DesktopStartupError(
                                    category == "service_readiness"
                                        ? arena::application::DesktopStartupFailureCategory::service_readiness
                                        : arena::application::DesktopStartupFailureCategory::pair_helper,
                                    failure.value(
                                        "detail",
                                        "the authenticated desktop helper failed"));
                            }
                        } catch (const DesktopStartupError&) {
                            throw;
                        } catch (const nlohmann::json::exception&) {
                        }
                        throw DesktopStartupError(
                            arena::application::DesktopStartupFailureCategory::pair_helper,
                            "the authenticated desktop helper returned an invalid result");
                    }
                }
                if (WaitForSingleObject(process_, 0) == WAIT_OBJECT_0) {
                    throw DesktopStartupError(
                        arena::application::DesktopStartupFailureCategory::pair_helper,
                        "the authenticated desktop helper exited before pairing");
                }
                return arena::application::ManagerStartupProbe{
                    .state = arena::application::ManagerStartupProbeState::pending,
                    .detail = "waiting for authenticated Manager core readiness",
                };
            },
            [manager] {
                return manager == nullptr
                    ? std::optional<std::uint32_t>{}
                    : manager->exit_code();
            },
            [] { return std::chrono::steady_clock::now(); },
            [](const std::chrono::milliseconds duration) {
                std::this_thread::sleep_for(duration);
            },
            arena::application::manager_core_readiness_deadline +
                std::chrono::seconds(10));
        if (result.state ==
            arena::application::ManagerStartupWaitState::service_exited) {
            throw DesktopStartupError(
                arena::application::DesktopStartupFailureCategory::service_process_exit,
                result.detail + " (exit code " +
                    std::to_string(result.service_exit_code.value_or(1)) + ")");
        }
        if (result.state != arena::application::ManagerStartupWaitState::ready ||
            url.empty()) {
            throw DesktopStartupError(
                arena::application::DesktopStartupFailureCategory::service_readiness,
                result.detail);
        }
        return url;
    }

private:
    void remove_scratch() noexcept {
        if (!scratch_path_.empty()) {
            std::error_code error;
            std::filesystem::remove_all(scratch_path_, error);
        }
    }

    static void close_handle(HANDLE& handle) {
        if (handle != nullptr && handle != INVALID_HANDLE_VALUE) {
            CloseHandle(handle);
        }
        handle = nullptr;
    }

    HANDLE process_{};
    HANDLE job_{};
    HANDLE output_read_{};
    std::filesystem::path scratch_path_;
};

int run_desktop() {
    HANDLE mutex =
        CreateMutexW(nullptr, FALSE, kLauncherMutex);
    if (mutex == nullptr) {
        return show_error(L"Cannot initialize the interactive Manager session.");
    }
    if (arena::application::desktop_launch_disposition(
            GetLastError() == ERROR_ALREADY_EXISTS) ==
        arena::application::DesktopLaunchDisposition::activate_existing) {
        activate_existing_window();
        CloseHandle(mutex);
        return 0;
    }

    try {
        try {
            configure_webview_user_data();
        } catch (const std::exception& error) {
            throw DesktopStartupError(
                arena::application::DesktopStartupFailureCategory::webview_initialization,
                error.what());
        }

        std::filesystem::path executable_root;
        try {
            executable_root = executable_directory();
        } catch (const std::exception& error) {
            throw DesktopStartupError(
                arena::application::DesktopStartupFailureCategory::service_launch,
                error.what());
        }
        std::unique_ptr<PerUserManagerProcess> manager;
        try {
            if (arena::platform::is_windows_per_user_install()) {
                manager = std::make_unique<PerUserManagerProcess>(
                    executable_root);
            }
        } catch (const std::exception& error) {
            throw DesktopStartupError(
                arena::application::DesktopStartupFailureCategory::service_launch,
                error.what());
        }
        const auto cli = executable_root / L"arena.exe";
        if (!std::filesystem::is_regular_file(cli)) {
            throw DesktopStartupError(
                arena::application::DesktopStartupFailureCategory::pair_helper,
                "arena.exe is missing; repair UCI Arena Suite Setup");
        }
        std::unique_ptr<UiHelper> helper;
        try {
            helper = std::make_unique<UiHelper>(
                cli, find_available_picker_port());
        } catch (const std::exception& error) {
            throw DesktopStartupError(
                arena::application::DesktopStartupFailureCategory::pair_helper,
                error.what());
        }
        const std::string url = helper->pairing_url(manager.get());

        try {
            webview::webview window(false, nullptr);
            window.set_title("UCI Arena Manager");
            window.set_size(1280, 820, WEBVIEW_HINT_NONE);
            if (const auto native_window = window.window();
                native_window.ok()) {
                const HICON icon = LoadIconW(
                    GetModuleHandleW(nullptr), MAKEINTRESOURCEW(101));
                if (icon != nullptr) {
                    SendMessageW(
                        static_cast<HWND>(native_window.value()),
                        WM_SETICON, ICON_BIG,
                        reinterpret_cast<LPARAM>(icon));
                    SendMessageW(
                        static_cast<HWND>(native_window.value()),
                        WM_SETICON, ICON_SMALL,
                        reinterpret_cast<LPARAM>(icon));
                }
            }
            window.navigate(url);
            window.run();
        } catch (const std::exception& error) {
            throw DesktopStartupError(
                arena::application::DesktopStartupFailureCategory::webview_initialization,
                error.what());
        }
        CloseHandle(mutex);
        return 0;
    } catch (const DesktopStartupError& error) {
        CloseHandle(mutex);
        return show_startup_error(error);
    } catch (const std::exception& error) {
        CloseHandle(mutex);
        return show_startup_error(DesktopStartupError(
            arena::application::DesktopStartupFailureCategory::webview_initialization,
            error.what()));
    }
}

}  // namespace

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    return run_desktop();
}

#else

#include <cerrno>
#include <csignal>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/file.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

namespace {

std::uint16_t find_available_picker_port() {
    const int listener = socket(AF_INET, SOCK_STREAM, 0);
    if (listener < 0) {
        throw std::runtime_error(
            "cannot allocate the native helper endpoint");
    }
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = 0;
    socklen_t address_size = sizeof(address);
    const bool bound =
        bind(
            listener,
            reinterpret_cast<const sockaddr*>(&address),
            sizeof(address)) == 0;
    const bool resolved =
        bound &&
        getsockname(
            listener,
            reinterpret_cast<sockaddr*>(&address),
            &address_size) == 0;
    close(listener);
    const std::uint16_t port = ntohs(address.sin_port);
    if (!resolved || port < 1024) {
        throw std::runtime_error(
            "cannot select the native helper endpoint");
    }
    return port;
}

class UiHelper final {
public:
    UiHelper(
        const std::filesystem::path& cli,
        const std::uint16_t picker_port) {
        int output[2]{};
        if (pipe(output) != 0) {
            throw std::runtime_error("cannot create the desktop helper channel");
        }
        process_ = fork();
        if (process_ == 0) {
            dup2(output[1], STDOUT_FILENO);
            const int null_handle = open("/dev/null", O_RDWR);
            if (null_handle >= 0) {
                dup2(null_handle, STDIN_FILENO);
                dup2(null_handle, STDERR_FILENO);
                close(null_handle);
            }
            close(output[0]);
            close(output[1]);
            const std::string picker_port_text =
                std::to_string(picker_port);
            execl(
                cli.c_str(), cli.c_str(), "ui", "--no-open", "--desktop-host",
                "--picker-port", picker_port_text.c_str(),
                static_cast<char*>(nullptr));
            _exit(127);
        }
        close(output[1]);
        if (process_ < 0) {
            close(output[0]);
            throw std::runtime_error(
                "cannot start the authenticated desktop helper");
        }
        output_ = output[0];
        scratch_path_ =
            std::filesystem::temp_directory_path() /
            ("uci-arena-manager-ui-" +
             std::to_string(process_));
        fcntl(output_, F_SETFL, fcntl(output_, F_GETFL) | O_NONBLOCK);
    }

    UiHelper(const UiHelper&) = delete;
    UiHelper& operator=(const UiHelper&) = delete;

    ~UiHelper() {
        if (process_ > 0) {
            kill(process_, SIGTERM);
            for (int attempt = 0; attempt < 30; ++attempt) {
                if (waitpid(process_, nullptr, WNOHANG) == process_) {
                    process_ = -1;
                    break;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }
            if (process_ > 0) {
                kill(process_, SIGKILL);
                waitpid(process_, nullptr, 0);
            }
        }
        if (output_ >= 0) {
            close(output_);
        }
        std::error_code error;
        std::filesystem::remove_all(scratch_path_, error);
    }

    std::string pairing_url() {
        std::string output;
        const auto deadline =
            std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (std::chrono::steady_clock::now() < deadline) {
            pollfd descriptor{output_, POLLIN, 0};
            if (poll(&descriptor, 1, 50) > 0 &&
                (descriptor.revents & POLLIN) != 0) {
                char buffer[1024];
                const ssize_t count = read(output_, buffer, sizeof(buffer));
                if (count > 0) {
                    output.append(buffer, static_cast<std::size_t>(count));
                    if (const auto newline = output.find('\n');
                        newline != std::string::npos) {
                        const std::string url = output.substr(0, newline);
                        if (!url.starts_with(
                                "http://127.0.0.1:47831/#pair=") ||
                            url.size() > 4096) {
                            throw std::runtime_error(
                                "the authenticated desktop helper returned an "
                                "invalid endpoint");
                        }
                        return url;
                    }
                }
            }
            int status = 0;
            if (waitpid(process_, &status, WNOHANG) == process_) {
                process_ = -1;
                throw std::runtime_error(
                    "the Manager service is unavailable to the desktop helper");
            }
        }
        throw std::runtime_error(
            "timed out while authenticating the desktop application");
    }

private:
    pid_t process_{-1};
    int output_{-1};
    std::filesystem::path scratch_path_;
};

std::filesystem::path executable_directory() {
    return std::filesystem::canonical("/proc/self/exe").parent_path();
}

int run_desktop() {
    const char* runtime_root = std::getenv("XDG_RUNTIME_DIR");
    const std::filesystem::path lock_path =
        (runtime_root != nullptr && *runtime_root != '\0'
             ? std::filesystem::path(runtime_root)
             : std::filesystem::temp_directory_path()) /
        ("uci-arena-manager-" + std::to_string(geteuid()) + ".lock");
    const int lock_handle =
        open(lock_path.c_str(), O_CREAT | O_RDWR | O_CLOEXEC, 0600);
    if (lock_handle < 0 || flock(lock_handle, LOCK_EX | LOCK_NB) != 0) {
        if (lock_handle >= 0) {
            close(lock_handle);
        }
        return 0;
    }

    try {
        const auto cli = executable_directory() / "arena";
        UiHelper helper(cli, find_available_picker_port());
        const std::string url = helper.pairing_url();
        webview::webview window(false, nullptr);
        window.set_title("UCI Arena Manager");
        window.set_size(1280, 820, WEBVIEW_HINT_NONE);
        window.navigate(url);
        window.run();
        close(lock_handle);
        return 0;
    } catch (const std::exception& error) {
        close(lock_handle);
        const std::string detail =
            std::string("UCI Arena Manager desktop startup failed: ") +
            error.what() + "\n";
        write(STDERR_FILENO, detail.data(), detail.size());
        return 1;
    }
}

}  // namespace

int main() {
    return run_desktop();
}

#endif
