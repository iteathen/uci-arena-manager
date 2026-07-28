#pragma once

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace arena::ports {

class NativeProcess final {
  public:
    NativeProcess();
    ~NativeProcess();

    NativeProcess(const NativeProcess&) = delete;
    NativeProcess& operator=(const NativeProcess&) = delete;

    void start(
        const std::filesystem::path& executable,
        const std::filesystem::path& working_directory,
        const std::vector<std::string>& arguments);
    void write_line(const std::string& line);
    [[nodiscard]] std::optional<std::string> read_line(std::chrono::milliseconds timeout);
    [[nodiscard]] bool running() const;
    [[nodiscard]] std::uint64_t pid() const;
    [[nodiscard]] std::uint64_t dropped_observational_lines() const;
    void stop(std::chrono::milliseconds graceful_timeout);

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace arena::ports
