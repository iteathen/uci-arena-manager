#pragma once

#include "arena/ports/native_process.hpp"

#include <chrono>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

namespace arena::application {

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
        std::chrono::milliseconds timeout = std::chrono::seconds(15)) const;

  private:
    mutable std::mutex mutex_;
    std::map<std::string, std::unique_ptr<ports::NativeProcess>, std::less<>> processes_;
};

}  // namespace arena::application
