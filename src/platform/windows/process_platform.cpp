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

constexpr std::size_t hard_maximum_line_bytes = 16 * 1024 * 1024;
constexpr std::size_t hard_maximum_queued_bytes = 64 * 1024 * 1024;
constexpr std::size_t hard_maximum_queued_lines = 65'536;
constexpr std::size_t hard_maximum_prefix_bytes = 64;

void validate_limits(const NativeProcessIoLimits& limits) {
    if (limits.maximum_input_line_bytes == 0 ||
        limits.maximum_input_line_bytes > hard_maximum_line_bytes) {
        throw std::runtime_error("native process input line bound is invalid");
    }
    if (limits.maximum_output_line_bytes == 0 ||
        limits.maximum_output_line_bytes > hard_maximum_line_bytes) {
        throw std::runtime_error("native process output line bound is invalid");
    }
    if (limits.maximum_queued_lines == 0 ||
        limits.maximum_queued_lines > hard_maximum_queued_lines) {
        throw std::runtime_error("native process queued line bound is invalid");
    }
    if (limits.maximum_queued_bytes < limits.maximum_output_line_bytes ||
        limits.maximum_queued_bytes > hard_maximum_queued_bytes) {
        throw std::runtime_error("native process queued byte bound is invalid");
    }
    if (limits.droppable_output_prefix.size() > hard_maximum_prefix_bytes ||
        limits.droppable_output_prefix.size() > limits.maximum_output_line_bytes ||
        limits.droppable_output_prefix.find_first_of("\0\r\n") != std::string::npos) {
        throw std::runtime_error("native process droppable prefix is invalid");
    }
}

bool invalid_input_line(const std::string_view line) {
    return line.find('\0') != std::string_view::npos ||
           line.find('\r') != std::string_view::npos ||
           line.find('\n') != std::string_view::npos;
}

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
    if (MultiByteToWideChar(
            CP_UTF8,
            MB_ERR_INVALID_CHARS,
            value.data(),
            static_cast<int>(value.size()),
            result.data(),
            size) != size) {
        throw std::runtime_error("failed to convert process argument");
    }
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

void reset_failed_start(
    HANDLE& job,
    HANDLE& process,
    HANDLE& thread,
    HANDLE& stdin_write,
    HANDLE& stdout_read,
    std::uint64_t& process_id) noexcept {
    if (job != nullptr) {
        static_cast<void>(TerminateJobObject(job, 1));
    } else if (process != nullptr) {
        static_cast<void>(TerminateProcess(process, 1));
    }
    if (process != nullptr) {
        static_cast<void>(WaitForSingleObject(process, 2'000));
    }
    close_handle(stdin_write);
    close_handle(stdout_read);
    close_handle(thread);
    close_handle(process);
    close_handle(job);
    process_id = 0;
}

}  // namespace

struct NativeProcess::Impl {
    HANDLE process{nullptr};
    HANDLE thread{nullptr};
    HANDLE job{nullptr};
    HANDLE stdin_write{nullptr};
    HANDLE stdout_read{nullptr};
    std::thread reader;
    mutable std::mutex mutex;
    std::condition_variable condition;
    std::deque<std::string> lines;
    std::size_t queued_bytes{};
    std::uint64_t process_id{};
    std::uint64_t dropped{};
    bool reader_finished{true};
    NativeProcessIoLimits limits{};
    std::string output_error;

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

    [[nodiscard]] bool droppable(const std::string_view line) const noexcept {
        return !limits.droppable_output_prefix.empty() &&
               line.starts_with(limits.droppable_output_prefix);
    }

    void fail_output_locked(const std::string_view reason) {
        if (output_error.empty()) {
            output_error.assign(reason);
        }
        condition.notify_all();
    }

    void record_oversized_line(const std::string_view prefix) {
        std::scoped_lock lock(mutex);
        if (droppable(prefix)) {
            ++dropped;
        } else {
            fail_output_locked("process control output line exceeds byte bound");
        }
    }

    void enqueue(std::string line) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }

        std::scoped_lock lock(mutex);
        if (!output_error.empty()) {
            return;
        }
        if (line.size() > limits.maximum_output_line_bytes) {
            if (droppable(line)) {
                ++dropped;
            } else {
                fail_output_locked("process control output line exceeds byte bound");
            }
            return;
        }

        while (lines.size() >= limits.maximum_queued_lines ||
               queued_bytes + line.size() > limits.maximum_queued_bytes) {
            const auto observational = std::find_if(
                lines.begin(), lines.end(), [this](const std::string& queued) {
                    return droppable(queued);
                });
            if (observational != lines.end()) {
                queued_bytes -= observational->size();
                lines.erase(observational);
                ++dropped;
                continue;
            }
            if (droppable(line)) {
                ++dropped;
                return;
            }
            fail_output_locked("process control output queue exceeds bound");
            return;
        }

        queued_bytes += line.size();
        lines.push_back(std::move(line));
        condition.notify_one();
    }

    void read_output() {
        std::string pending;
        bool discard_until_newline = false;
        const std::size_t wire_line_limit = limits.maximum_output_line_bytes + 1;
        char buffer[4096];

        for (;;) {
            DWORD count = 0;
            if (!ReadFile(stdout_read, buffer, sizeof(buffer), &count, nullptr)) {
                const DWORD code = GetLastError();
                if (code != ERROR_BROKEN_PIPE && code != ERROR_OPERATION_ABORTED) {
                    std::scoped_lock lock(mutex);
                    fail_output_locked("failed to read process output");
                }
                break;
            }
            if (count == 0) {
                break;
            }

            std::size_t offset = 0;
            while (offset < count) {
                const std::string_view chunk(buffer + offset, count - offset);
                const std::size_t newline = chunk.find('\n');
                const std::size_t segment_size =
                    newline == std::string_view::npos ? chunk.size() : newline;

                if (!discard_until_newline) {
                    const std::size_t available =
                        wire_line_limit > pending.size()
                            ? wire_line_limit - pending.size()
                            : 0;
                    const std::size_t accepted = std::min(available, segment_size);
                    pending.append(chunk.data(), accepted);
                    if (accepted != segment_size) {
                        record_oversized_line(pending);
                        pending.clear();
                        discard_until_newline = true;
                    }
                }

                offset += segment_size;
                if (newline == std::string_view::npos) {
                    break;
                }
                if (!discard_until_newline) {
                    enqueue(std::move(pending));
                }
                pending.clear();
                discard_until_newline = false;
                ++offset;
            }
        }

        if (!discard_until_newline && !pending.empty()) {
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
    const std::vector<std::string>& arguments,
    const NativeProcessIoLimits& limits) {
    if (running()) {
        throw std::runtime_error("process is already running");
    }
    validate_limits(limits);

    SECURITY_ATTRIBUTES security{
        .nLength = sizeof(SECURITY_ATTRIBUTES),
        .lpSecurityDescriptor = nullptr,
        .bInheritHandle = TRUE,
    };
    HANDLE stdin_read = nullptr;
    HANDLE stdout_write = nullptr;
    if (!CreatePipe(&stdin_read, &impl_->stdin_write, &security, 0)) {
        throw std::runtime_error("failed to create process input pipe");
    }
    if (!CreatePipe(&impl_->stdout_read, &stdout_write, &security, 0)) {
        close_handle(stdin_read);
        close_handle(impl_->stdin_write);
        throw std::runtime_error("failed to create process output pipe");
    }
    if (!SetHandleInformation(impl_->stdin_write, HANDLE_FLAG_INHERIT, 0) ||
        !SetHandleInformation(impl_->stdout_read, HANDLE_FLAG_INHERIT, 0)) {
        close_handle(stdin_read);
        close_handle(impl_->stdin_write);
        close_handle(impl_->stdout_read);
        close_handle(stdout_write);
        throw std::runtime_error("failed to secure process pipe inheritance");
    }

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
        close_handle(impl_->stdin_write);
        close_handle(impl_->stdout_read);
        throw std::runtime_error("failed to create process Job Object");
    }
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION job_limits{};
    job_limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (!SetInformationJobObject(
            impl_->job,
            JobObjectExtendedLimitInformation,
            &job_limits,
            sizeof(job_limits)) ||
        !AssignProcessToJobObject(impl_->job, process_info.hProcess)) {
        TerminateProcess(process_info.hProcess, 1);
        CloseHandle(process_info.hThread);
        CloseHandle(process_info.hProcess);
        close_handle(impl_->job);
        close_handle(impl_->stdin_write);
        close_handle(impl_->stdout_read);
        throw std::runtime_error("failed to contain process in Job Object");
    }

    impl_->process = process_info.hProcess;
    impl_->thread = process_info.hThread;
    impl_->process_id = process_info.dwProcessId;
    {
        std::scoped_lock lock(impl_->mutex);
        impl_->limits = limits;
        impl_->lines.clear();
        impl_->queued_bytes = 0;
        impl_->dropped = 0;
        impl_->output_error.clear();
        impl_->reader_finished = false;
    }
    if (ResumeThread(impl_->thread) == static_cast<DWORD>(-1)) {
        reset_failed_start(
            impl_->job,
            impl_->process,
            impl_->thread,
            impl_->stdin_write,
            impl_->stdout_read,
            impl_->process_id);
        {
            std::scoped_lock lock(impl_->mutex);
            impl_->reader_finished = true;
            impl_->lines.clear();
            impl_->queued_bytes = 0;
        }
        throw std::runtime_error("failed to resume process thread");
    }
    try {
        impl_->reader = std::thread([this] { impl_->read_output(); });
    } catch (...) {
        reset_failed_start(
            impl_->job,
            impl_->process,
            impl_->thread,
            impl_->stdin_write,
            impl_->stdout_read,
            impl_->process_id);
        {
            std::scoped_lock lock(impl_->mutex);
            impl_->reader_finished = true;
            impl_->lines.clear();
            impl_->queued_bytes = 0;
        }
        throw;
    }
}

void NativeProcess::write_line(const std::string& line) {
    if (!running()) {
        throw std::runtime_error("cannot write to a stopped process");
    }
    if (line.size() > impl_->limits.maximum_input_line_bytes) {
        throw std::runtime_error("process input line exceeds byte bound");
    }
    if (invalid_input_line(line)) {
        throw std::runtime_error("process input line contains a framing character");
    }

    const std::string framed = line + "\n";
    std::size_t offset = 0;
    while (offset < framed.size()) {
        DWORD written = 0;
        const DWORD remaining = static_cast<DWORD>(framed.size() - offset);
        if (!WriteFile(
                impl_->stdin_write,
                framed.data() + offset,
                remaining,
                &written,
                nullptr) ||
            written == 0) {
            throw std::runtime_error("failed to write to process");
        }
        offset += static_cast<std::size_t>(written);
    }
}

std::optional<std::string> NativeProcess::read_line(std::chrono::milliseconds timeout) {
    if (timeout.count() < 0) {
        throw std::runtime_error("process read timeout is invalid");
    }
    std::unique_lock lock(impl_->mutex);
    impl_->condition.wait_for(lock, timeout, [this] {
        return !impl_->output_error.empty() ||
               !impl_->lines.empty() ||
               impl_->reader_finished;
    });
    if (!impl_->output_error.empty()) {
        throw std::runtime_error(impl_->output_error);
    }
    if (impl_->lines.empty()) {
        return std::nullopt;
    }
    std::string line = std::move(impl_->lines.front());
    impl_->queued_bytes -= line.size();
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
    if (graceful_timeout.count() < 0) {
        throw std::runtime_error("process stop timeout is invalid");
    }
    if (running()) {
        try {
            write_line("quit");
        } catch (...) {
        }
        if (WaitForSingleObject(
                impl_->process,
                static_cast<DWORD>(graceful_timeout.count())) == WAIT_TIMEOUT) {
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
