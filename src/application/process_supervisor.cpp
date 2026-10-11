#include "arena/application/process_supervisor.hpp"

#include <stdexcept>
#include <thread>

namespace arena::application {

ProcessCommandFailure::ProcessCommandFailure(
    const ProcessCommandFailureKind kind,
    const std::string& detail)
    : std::runtime_error(detail), kind_(kind) {}

ProcessCommandFailureKind ProcessCommandFailure::kind() const noexcept {
    return kind_;
}

ProcessSupervisor::~ProcessSupervisor() {
    std::map<std::string, std::unique_ptr<ports::NativeProcess>, std::less<>> processes;
    {
        std::scoped_lock lock(mutex_);
        processes.swap(processes_);
    }
    for (auto& [id, process] : processes) {
        (void)id;
        try {
            process->stop(std::chrono::seconds(2));
        } catch (...) {
        }
    }
}

void ProcessSupervisor::start(
    const std::string& resource_id,
    const std::filesystem::path& executable,
    const std::filesystem::path& working_directory,
    const std::vector<std::string>& arguments) {
    auto process = std::make_unique<ports::NativeProcess>();
    process->start(executable, working_directory, arguments);
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    if (!process->running()) {
        process->stop(std::chrono::milliseconds(100));
        throw std::runtime_error("managed service exited during startup");
    }
    std::scoped_lock lock(mutex_);
    const auto existing = processes_.find(resource_id);
    if (existing != processes_.end() && existing->second->running()) {
        process->stop(std::chrono::milliseconds(250));
        throw std::runtime_error("managed service is already running");
    }
    processes_[resource_id] = std::move(process);
}

void ProcessSupervisor::stop(
    const std::string& resource_id,
    std::chrono::milliseconds graceful_timeout) {
    std::unique_ptr<ports::NativeProcess> process;
    {
        std::scoped_lock lock(mutex_);
        const auto existing = processes_.find(resource_id);
        if (existing == processes_.end()) {
            return;
        }
        process = std::move(existing->second);
        processes_.erase(existing);
    }
    process->stop(graceful_timeout);
}

bool ProcessSupervisor::running(const std::string& resource_id) const {
    std::scoped_lock lock(mutex_);
    const auto process = processes_.find(resource_id);
    return process != processes_.end() && process->second->running();
}

nlohmann::json ProcessSupervisor::run_json(
    const std::filesystem::path& executable,
    const std::filesystem::path& working_directory,
    const std::vector<std::string>& arguments,
    std::chrono::milliseconds timeout,
    const std::size_t maximum_output_bytes,
    const bool require_successful_exit) const {
    if (maximum_output_bytes == 0) {
        throw ProcessCommandFailure(
            ProcessCommandFailureKind::output_limit,
            "managed service output limit is invalid");
    }
    ports::NativeProcess process;
    try {
        process.start(executable, working_directory, arguments);
    } catch (const std::exception& error) {
        throw ProcessCommandFailure(
            ProcessCommandFailureKind::launch,
            std::string("managed service command could not be launched: ") +
                error.what());
    }
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    std::string output;
    const auto append_line = [&](const std::string& line) {
        if (line.size() >= maximum_output_bytes ||
            output.size() > maximum_output_bytes - line.size() - 1) {
            throw ProcessCommandFailure(
                ProcessCommandFailureKind::output_limit,
                "managed service output exceeded the one MiB contract boundary");
        }
        output += line;
        output.push_back('\n');
    };
    while (std::chrono::steady_clock::now() < deadline) {
        if (const auto line = process.read_line(std::chrono::milliseconds(100)); line) {
            append_line(*line);
            continue;
        }
        if (!process.running()) {
            break;
        }
    }
    if (process.running()) {
        process.stop(std::chrono::milliseconds(250));
        throw ProcessCommandFailure(
            ProcessCommandFailureKind::timeout,
            "managed service command timed out");
    }
    while (const auto line = process.read_line(std::chrono::milliseconds(0))) {
        append_line(*line);
    }
    const auto completed_exit = require_successful_exit ? process.exit_code() : std::nullopt;
    process.stop(std::chrono::milliseconds(0));
    if (process.dropped_observational_lines() != 0) {
        throw ProcessCommandFailure(
            ProcessCommandFailureKind::output_limit,
            "managed service output exceeded the bounded transport queue");
    }
    if (require_successful_exit && (!completed_exit || *completed_exit != 0)) {
        throw ProcessCommandFailure(
            ProcessCommandFailureKind::unsuccessful_exit,
            "managed command did not complete successfully");
    }
    try {
        return nlohmann::json::parse(output);
    } catch (const nlohmann::json::exception&) {
        throw ProcessCommandFailure(
            ProcessCommandFailureKind::invalid_output,
            "managed service returned invalid machine-readable status");
    }
}

}  // namespace arena::application
