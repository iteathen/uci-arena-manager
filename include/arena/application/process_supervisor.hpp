#pragma once

#include "arena/ports/native_process.hpp"

#include <chrono>
#include <cstddef>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <nlohmann/json.hpp>
#include <string>
#include <stdexcept>
#include <vector>

namespace arena::application {

enum class ProcessCommandFailureKind {
    launch,
    timeout,
    output_limit,
    invalid_output,
};

class ProcessCommandFailure final : public std::runtime_error {
  public:
    ProcessCommandFailure(
        ProcessCommandFailureKind kind,
        const std::string& detail);
    [[nodiscard]] ProcessCommandFailureKind kind() const noexcept;

  private:
    ProcessCommandFailureKind kind_;
};

class ProcessSupervisor final {
  public:
    ~ProcessSupervisor();

    void start(
        const std::string& resource_id,
        const std::filesystem::path& executable,
        const std::filesystem::path& working_directory,
        const std::vector<std::string>& arguments);
    void stop(
        const std::string& resource_id,
        std::chrono::milliseconds graceful_timeout = std::chrono::seconds(5));
    [[nodiscard]] bool running(const std::string& resource_id) const;
    [[nodiscard]] nlohmann::json run_json(
        const std::filesystem::path& executable,
        const std::filesystem::path& working_directory,
        const std::vector<std::string>& arguments,
        std::chrono::milliseconds timeout = std::chrono::seconds(15),
        std::size_t maximum_output_bytes = 1024 * 1024) const;

  private:
    mutable std::mutex mutex_;
    std::map<std::string, std::unique_ptr<ports::NativeProcess>, std::less<>> processes_;
};

}  // namespace arena::application
