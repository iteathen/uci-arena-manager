#include "arena/application/process_supervisor.hpp"

#include <stdexcept>
#include <thread>

namespace arena::application {

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
    std::chrono::milliseconds timeout) const {
    ports::NativeProcess process;
    process.start(executable, working_directory, arguments);
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    std::string output;
    while (std::chrono::steady_clock::now() < deadline) {
        if (const auto line = process.read_line(std::chrono::milliseconds(100)); line) {
            output += *line;
            output.push_back('\n');
            continue;
        }
        if (!process.running()) {
            break;
        }
    }
    if (process.running()) {
        process.stop(std::chrono::milliseconds(250));
        throw std::runtime_error("managed service command timed out");
    }
    while (const auto line = process.read_line(std::chrono::milliseconds(0))) {
        output += *line;
        output.push_back('\n');
    }
    process.stop(std::chrono::milliseconds(0));
    try {
        return nlohmann::json::parse(output);
    } catch (const nlohmann::json::exception&) {
        throw std::runtime_error("managed service returned invalid machine-readable status");
    }
}

}  // namespace arena::application
