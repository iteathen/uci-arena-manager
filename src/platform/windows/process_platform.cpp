#include "arena/ports/native_process.hpp"

#define NOMINMAX
#include <windows.h>

#include <algorithm>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <stdexcept>
#include <string_view>
#include <thread>

namespace arena::ports {
namespace {

std::wstring utf8_to_wide(const std::string& value) {
    if (value.empty()) {
        return {};
    }
    const int size = MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), nullptr, 0);
    if (size <= 0) {
        throw std::runtime_error("invalid UTF-8 process argument");
    }
    std::wstring result(static_cast<std::size_t>(size), L'\0');
    MultiByteToWideChar(
        CP_UTF8,
        MB_ERR_INVALID_CHARS,
        value.data(),
        static_cast<int>(value.size()),
        result.data(),
        size);
    return result;
}

std::wstring quote_argument(const std::wstring& value) {
    if (value.find_first_of(L" \t\n\v\"") == std::wstring::npos) {
        return value;
    }
    std::wstring result{L"\""};
    std::size_t slashes = 0;
    for (const wchar_t character : value) {
        if (character == L'\\') {
            ++slashes;
            continue;
        }
        if (character == L'"') {
            result.append(slashes * 2 + 1, L'\\');
            result.push_back(L'"');
            slashes = 0;
            continue;
        }
        result.append(slashes, L'\\');
        slashes = 0;
        result.push_back(character);
    }
    result.append(slashes * 2, L'\\');
    result.push_back(L'"');
    return result;
}

void close_handle(HANDLE& handle) {
    if (handle != nullptr && handle != INVALID_HANDLE_VALUE) {
        CloseHandle(handle);
        handle = nullptr;
    }
}

}  // namespace

struct NativeProcess::Impl {
    static constexpr std::size_t max_lines = 8192;

    HANDLE process{nullptr};
    HANDLE thread{nullptr};
    HANDLE job{nullptr};
    HANDLE stdin_write{nullptr};
    HANDLE stdout_read{nullptr};
    std::thread reader;
    mutable std::mutex mutex;
    std::condition_variable condition;
    std::deque<std::string> lines;
    std::uint64_t process_id{};
    std::uint64_t dropped{};
    bool reader_finished{true};

    ~Impl() {
        if (reader.joinable()) {
            close_handle(stdout_read);
            reader.join();
        }
        close_handle(stdin_write);
        close_handle(stdout_read);
        close_handle(thread);
        close_handle(process);
        close_handle(job);
    }

    void enqueue(std::string line) {
        std::scoped_lock lock(mutex);
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        if (lines.size() >= max_lines) {
            if (line.starts_with("info ")) {
                ++dropped;
                return;
            }
            const auto observational = std::find_if(
                lines.begin(), lines.end(), [](const std::string& queued) {
                    return queued.starts_with("info ");
                });
            if (observational != lines.end()) {
                lines.erase(observational);
                ++dropped;
            } else {
                lines.pop_front();
                ++dropped;
            }
        }
        lines.push_back(std::move(line));
        condition.notify_one();
    }

    void read_output() {
        std::string pending;
        char buffer[4096];
        DWORD read = 0;
        while (ReadFile(stdout_read, buffer, sizeof(buffer), &read, nullptr) && read != 0) {
            pending.append(buffer, read);
            std::size_t newline = 0;
            while ((newline = pending.find('\n')) != std::string::npos) {
                enqueue(pending.substr(0, newline));
                pending.erase(0, newline + 1);
            }
        }
        if (!pending.empty()) {
            enqueue(std::move(pending));
        }
        {
            std::scoped_lock lock(mutex);
            reader_finished = true;
        }
        condition.notify_all();
    }
};

NativeProcess::NativeProcess() : impl_(std::make_unique<Impl>()) {}

NativeProcess::~NativeProcess() {
    try {
        stop(std::chrono::milliseconds(250));
    } catch (...) {
    }
}

void NativeProcess::start(
    const std::filesystem::path& executable,
    const std::filesystem::path& working_directory,
    const std::vector<std::string>& arguments) {
    if (running()) {
        throw std::runtime_error("process is already running");
    }

    SECURITY_ATTRIBUTES security{
        .nLength = sizeof(SECURITY_ATTRIBUTES),
        .lpSecurityDescriptor = nullptr,
        .bInheritHandle = TRUE,
    };
    HANDLE stdin_read = nullptr;
    HANDLE stdout_write = nullptr;
    if (!CreatePipe(&stdin_read, &impl_->stdin_write, &security, 0) ||
        !CreatePipe(&impl_->stdout_read, &stdout_write, &security, 0)) {
        close_handle(stdin_read);
        close_handle(stdout_write);
        throw std::runtime_error("failed to create process pipes");
    }
    SetHandleInformation(impl_->stdin_write, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(impl_->stdout_read, HANDLE_FLAG_INHERIT, 0);

    const std::wstring executable_wide = executable.wstring();
    std::wstring command_line = quote_argument(executable_wide);
    for (const auto& argument : arguments) {
        command_line.push_back(L' ');
        command_line += quote_argument(utf8_to_wide(argument));
    }

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = stdin_read;
    startup.hStdOutput = stdout_write;
    startup.hStdError = stdout_write;
    PROCESS_INFORMATION process_info{};
    std::wstring working_directory_wide = working_directory.wstring();

    const BOOL created = CreateProcessW(
        executable_wide.c_str(),
        command_line.data(),
        nullptr,
        nullptr,
        TRUE,
        CREATE_NO_WINDOW | CREATE_SUSPENDED | CREATE_UNICODE_ENVIRONMENT,
        nullptr,
        working_directory_wide.c_str(),
        &startup,
        &process_info);
    close_handle(stdin_read);
    close_handle(stdout_write);
    if (!created) {
        close_handle(impl_->stdin_write);
        close_handle(impl_->stdout_read);
        throw std::runtime_error("failed to launch process, Windows error " +
                                 std::to_string(GetLastError()));
    }

    impl_->job = CreateJobObjectW(nullptr, nullptr);
    if (impl_->job == nullptr) {
        TerminateProcess(process_info.hProcess, 1);
        CloseHandle(process_info.hThread);
        CloseHandle(process_info.hProcess);
        throw std::runtime_error("failed to create process Job Object");
    }
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (!SetInformationJobObject(
            impl_->job,
            JobObjectExtendedLimitInformation,
            &limits,
            sizeof(limits)) ||
        !AssignProcessToJobObject(impl_->job, process_info.hProcess)) {
        TerminateProcess(process_info.hProcess, 1);
        CloseHandle(process_info.hThread);
        CloseHandle(process_info.hProcess);
        close_handle(impl_->job);
        throw std::runtime_error("failed to contain process in Job Object");
    }

    impl_->process = process_info.hProcess;
    impl_->thread = process_info.hThread;
    impl_->process_id = process_info.dwProcessId;
    {
        std::scoped_lock lock(impl_->mutex);
        impl_->lines.clear();
        impl_->dropped = 0;
        impl_->reader_finished = false;
    }
    ResumeThread(impl_->thread);
    impl_->reader = std::thread([this] { impl_->read_output(); });
}

void NativeProcess::write_line(const std::string& line) {
    if (!running()) {
        throw std::runtime_error("cannot write to a stopped process");
    }
    const std::string framed = line + "\n";
    DWORD written = 0;
    if (!WriteFile(
            impl_->stdin_write,
            framed.data(),
            static_cast<DWORD>(framed.size()),
            &written,
            nullptr) ||
        written != framed.size()) {
        throw std::runtime_error("failed to write to process");
    }
}

std::optional<std::string> NativeProcess::read_line(std::chrono::milliseconds timeout) {
    std::unique_lock lock(impl_->mutex);
    impl_->condition.wait_for(lock, timeout, [this] {
        return !impl_->lines.empty() || impl_->reader_finished;
    });
    if (impl_->lines.empty()) {
        return std::nullopt;
    }
    std::string line = std::move(impl_->lines.front());
    impl_->lines.pop_front();
    return line;
}

bool NativeProcess::running() const {
    if (impl_->process == nullptr) {
        return false;
    }
    DWORD exit_code = 0;
    return GetExitCodeProcess(impl_->process, &exit_code) && exit_code == STILL_ACTIVE;
}

std::uint64_t NativeProcess::pid() const { return impl_->process_id; }

std::uint64_t NativeProcess::dropped_observational_lines() const {
    std::scoped_lock lock(impl_->mutex);
    return impl_->dropped;
}

void NativeProcess::stop(std::chrono::milliseconds graceful_timeout) {
    if (impl_->process == nullptr) {
        return;
    }
    if (running()) {
        try {
            write_line("quit");
        } catch (...) {
        }
        if (WaitForSingleObject(impl_->process, static_cast<DWORD>(graceful_timeout.count())) ==
            WAIT_TIMEOUT) {
            TerminateJobObject(impl_->job, 1);
            WaitForSingleObject(impl_->process, 2000);
        }
    }
    close_handle(impl_->stdin_write);
    if (impl_->reader.joinable()) {
        impl_->reader.join();
    }
    close_handle(impl_->stdout_read);
    close_handle(impl_->thread);
    close_handle(impl_->process);
    close_handle(impl_->job);
    impl_->process_id = 0;
}

}  // namespace arena::ports
