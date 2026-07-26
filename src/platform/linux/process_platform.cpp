#include "arena/ports/native_process.hpp"

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <mutex>
#include <stdexcept>
#include <thread>

namespace arena::ports {

struct NativeProcess::Impl {
    static constexpr std::size_t max_lines = 8192;

    pid_t process_id{-1};
    int stdin_fd{-1};
    int stdout_fd{-1};
    std::thread reader;
    mutable std::mutex mutex;
    std::condition_variable condition;
    std::deque<std::string> lines;
    std::uint64_t dropped{};
    bool reader_finished{true};

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
            } else {
                lines.pop_front();
            }
            ++dropped;
        }
        lines.push_back(std::move(line));
        condition.notify_one();
    }

    void read_output() {
        std::string pending;
        char buffer[4096];
        ssize_t count = 0;
        while ((count = read(stdout_fd, buffer, sizeof(buffer))) > 0) {
            pending.append(buffer, static_cast<std::size_t>(count));
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
    int stdin_pipe[2]{};
    int stdout_pipe[2]{};
    if (pipe2(stdin_pipe, O_CLOEXEC) != 0 || pipe2(stdout_pipe, O_CLOEXEC) != 0) {
        throw std::runtime_error(std::string("failed to create process pipes: ") +
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
        impl_->lines.clear();
        impl_->dropped = 0;
        impl_->reader_finished = false;
    }
    impl_->reader = std::thread([this] { impl_->read_output(); });
}

void NativeProcess::write_line(const std::string& line) {
    if (!running()) {
        throw std::runtime_error("cannot write to a stopped process");
    }
    const std::string framed = line + "\n";
    std::size_t offset = 0;
    while (offset < framed.size()) {
        const ssize_t written = write(
            impl_->stdin_fd, framed.data() + offset, framed.size() - offset);
        if (written <= 0) {
            throw std::runtime_error("failed to write to process");
        }
        offset += static_cast<std::size_t>(written);
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
            kill(-impl_->process_id, SIGKILL);
            waitpid(impl_->process_id, nullptr, 0);
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
