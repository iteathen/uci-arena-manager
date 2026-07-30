#include "webview/webview.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <thread>

#ifdef _WIN32
#define NOMINMAX
#include <winsock2.h>
#include <shlobj.h>
#include <windows.h>

namespace {

constexpr wchar_t kWindowTitle[] = L"UCI Arena Manager";
constexpr wchar_t kLauncherMutex[] =
    L"Local\\UCIArenaManagerInteractiveLauncher";

std::wstring quote(const std::wstring& value) {
    return L"\"" + value + L"\"";
}

int show_error(const wchar_t* detail) {
    MessageBoxW(nullptr, detail, kWindowTitle, MB_OK | MB_ICONERROR);
    return 1;
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
            std::to_wstring(picker_port);
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
        close_handle(process_);
        close_handle(output_read_);
    }

    std::string pairing_url() {
        std::string output;
        const auto deadline =
            std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (std::chrono::steady_clock::now() < deadline) {
            DWORD available = 0;
            if (!PeekNamedPipe(
                    output_read_, nullptr, 0, nullptr, &available, nullptr)) {
                throw std::runtime_error(
                    "the authenticated desktop helper channel failed");
            }
            if (available > 0) {
                char buffer[1024];
                DWORD read = 0;
                if (!ReadFile(
                        output_read_, buffer,
                        static_cast<DWORD>(
                            std::min<std::size_t>(sizeof(buffer), available)),
                        &read, nullptr)) {
                    throw std::runtime_error(
                        "the authenticated desktop helper did not respond");
                }
                output.append(buffer, read);
                if (const auto newline = output.find('\n');
                    newline != std::string::npos) {
                    std::string url = output.substr(0, newline);
                    if (!url.empty() && url.back() == '\r') {
                        url.pop_back();
                    }
                    if (!url.starts_with(
                            "http://127.0.0.1:47831/#pair=") ||
                        url.size() > 4096) {
                        throw std::runtime_error(
                            "the authenticated desktop helper returned an "
                            "invalid endpoint");
                    }
                    return url;
                }
                if (output.size() > 4096) {
                    throw std::runtime_error(
                        "the authenticated desktop helper response was too large");
                }
            } else if (
                WaitForSingleObject(process_, 0) == WAIT_OBJECT_0) {
                throw std::runtime_error(
                    "the Manager service is unavailable to the desktop helper");
            } else {
                std::this_thread::sleep_for(std::chrono::milliseconds(25));
            }
        }
        throw std::runtime_error(
            "timed out while authenticating the desktop application");
    }

private:
    static void close_handle(HANDLE& handle) {
        if (handle != nullptr && handle != INVALID_HANDLE_VALUE) {
            CloseHandle(handle);
        }
        handle = nullptr;
    }

    HANDLE process_{};
    HANDLE job_{};
    HANDLE output_read_{};
};

int run_desktop() {
    HANDLE mutex =
        CreateMutexW(nullptr, FALSE, kLauncherMutex);
    if (mutex == nullptr) {
        return show_error(L"Cannot initialize the interactive Manager session.");
    }
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        activate_existing_window();
        CloseHandle(mutex);
        return 0;
    }

    try {
        configure_webview_user_data();
        const auto cli = executable_directory() / L"arena.exe";
        if (!std::filesystem::is_regular_file(cli)) {
            throw std::runtime_error(
                "arena.exe is missing; repair UCI Arena Suite Setup");
        }
        UiHelper helper(cli, find_available_picker_port());
        const std::string url = helper.pairing_url();

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
        CloseHandle(mutex);
        return 0;
    } catch (const std::exception&) {
        CloseHandle(mutex);
        return show_error(
            L"UCI Arena Manager could not open its native window. Verify that "
            L"the UCI Arena Manager service and Microsoft Edge WebView2 Runtime "
            L"are installed, then repair UCI Arena Suite Setup.");
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
