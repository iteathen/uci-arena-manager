#include "arena/ports/native_process.hpp"

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <condition_variable>
#include <cstring>
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

void reap_child(const pid_t child) noexcept {
    static_cast<void>(kill(-child, SIGKILL));
    static_cast<void>(kill(child, SIGKILL));
    while (waitpid(child, nullptr, 0) < 0 && errno == EINTR) {
    }
}

}  // namespace

struct NativeProcess::Impl {
    pid_t process_id{-1};
    int stdin_fd{-1};
    int stdout_fd{-1};
    std::thread reader;
    mutable std::mutex mutex;
    std::condition_variable condition;
    std::deque<std::string> lines;
    std::size_t queued_bytes{};
    std::uint64_t dropped{};
    bool reader_finished{true};
    NativeProcessIoLimits limits{};
    std::string output_error;

    [[nodiscard]] bool droppable(const std::string_view line) const noexcept {
        return !limits.droppable_output_prefix.empty() &&
               line.starts_with(limits.droppable_output_prefix);
    }

    void close_descriptors() {
        if (stdin_fd >= 0) {
            close(stdin_fd);
            stdin_fd = -1;
        }
        if (stdout_fd >= 0) {
            close(stdout_fd);
            stdout_fd = -1;
        }
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
            const ssize_t count = read(stdout_fd, buffer, sizeof(buffer));
            if (count < 0 && errno == EINTR) {
                continue;
            }
            if (count <= 0) {
                break;
            }

            const std::size_t read_size = static_cast<std::size_t>(count);
            std::size_t offset = 0;
            while (offset < read_size) {
                const std::string_view chunk(buffer + offset, read_size - offset);
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

    int stdin_pipe[2]{};
    int stdout_pipe[2]{};
    if (pipe2(stdin_pipe, O_CLOEXEC) != 0) {
        throw std::runtime_error(std::string("failed to create process input pipe: ") +
                                 std::strerror(errno));
    }
    if (pipe2(stdout_pipe, O_CLOEXEC) != 0) {
        close(stdin_pipe[0]);
        close(stdin_pipe[1]);
        throw std::runtime_error(std::string("failed to create process output pipe: ") +
                                 std::strerror(errno));
    }

    const pid_t child = fork();
    if (child < 0) {
        close(stdin_pipe[0]);
        close(stdin_pipe[1]);
        close(stdout_pipe[0]);
        close(stdout_pipe[1]);
        throw std::runtime_error(std::string("failed to fork: ") + std::strerror(errno));
    }
    if (child == 0) {
        setpgid(0, 0);
        if (chdir(working_directory.c_str()) != 0 ||
            dup2(stdin_pipe[0], STDIN_FILENO) < 0 ||
            dup2(stdout_pipe[1], STDOUT_FILENO) < 0 ||
            dup2(stdout_pipe[1], STDERR_FILENO) < 0) {
            _exit(126);
        }
        close(stdin_pipe[0]);
        close(stdin_pipe[1]);
        close(stdout_pipe[0]);
        close(stdout_pipe[1]);

        std::vector<std::string> owned_arguments;
        owned_arguments.reserve(arguments.size() + 1);
        owned_arguments.push_back(executable.string());
        owned_arguments.insert(owned_arguments.end(), arguments.begin(), arguments.end());
        std::vector<char*> argv;
        argv.reserve(owned_arguments.size() + 1);
        for (auto& argument : owned_arguments) {
            argv.push_back(argument.data());
        }
        argv.push_back(nullptr);
        execv(executable.c_str(), argv.data());
        _exit(127);
    }

    close(stdin_pipe[0]);
    close(stdout_pipe[1]);
    impl_->process_id = child;
    impl_->stdin_fd = stdin_pipe[1];
    impl_->stdout_fd = stdout_pipe[0];
    {
        std::scoped_lock lock(impl_->mutex);
        impl_->limits = limits;
        impl_->lines.clear();
        impl_->queued_bytes = 0;
        impl_->dropped = 0;
        impl_->output_error.clear();
        impl_->reader_finished = false;
    }
    try {
        impl_->reader = std::thread([this] { impl_->read_output(); });
    } catch (...) {
        reap_child(child);
        impl_->close_descriptors();
        impl_->process_id = -1;
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
        const ssize_t written = write(
            impl_->stdin_fd, framed.data() + offset, framed.size() - offset);
        if (written < 0 && errno == EINTR) {
            continue;
        }
        if (written <= 0) {
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
    if (impl_->process_id <= 0) {
        return false;
    }
    int status = 0;
    const pid_t result = waitpid(impl_->process_id, &status, WNOHANG);
    return result == 0;
}

std::uint64_t NativeProcess::pid() const {
    return impl_->process_id > 0 ? static_cast<std::uint64_t>(impl_->process_id) : 0;
}

std::uint64_t NativeProcess::dropped_observational_lines() const {
    std::scoped_lock lock(impl_->mutex);
    return impl_->dropped;
}

void NativeProcess::stop(std::chrono::milliseconds graceful_timeout) {
    if (impl_->process_id <= 0) {
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
        const auto deadline = std::chrono::steady_clock::now() + graceful_timeout;
        while (std::chrono::steady_clock::now() < deadline && running()) {
            poll(nullptr, 0, 10);
        }
        if (running()) {
            reap_child(impl_->process_id);
        }
    }
    if (impl_->stdin_fd >= 0) {
        close(impl_->stdin_fd);
        impl_->stdin_fd = -1;
    }
    if (impl_->reader.joinable()) {
        impl_->reader.join();
    }
    impl_->close_descriptors();
    impl_->process_id = -1;
}

}  // namespace arena::ports
