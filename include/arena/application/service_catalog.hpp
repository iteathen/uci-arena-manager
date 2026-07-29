#pragma once

#include "arena/application/process_supervisor.hpp"

#include <chrono>
#include <filesystem>
#include <functional>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

namespace arena::application {

class ServiceCatalog final {
  public:
    using BotCommandRunner = std::function<nlohmann::json(
        const std::filesystem::path&,
        const std::filesystem::path&,
        const std::vector<std::string>&,
        std::chrono::milliseconds)>;

    [[nodiscard]] static nlohmann::json profiles();
    [[nodiscard]] static nlohmann::json discover(
        const std::string& profile_id,
        const std::filesystem::path& manager_data_root = {});
    [[nodiscard]] static nlohmann::json normalize_registration(
        const nlohmann::json& parameters);
    [[nodiscard]] static nlohmann::json installed_registrations(
        const std::filesystem::path& manager_data_root);
    [[nodiscard]] static nlohmann::json component_inventory(
        const std::filesystem::path& manager_data_root);
    [[nodiscard]] static nlohmann::json bot_profiles();
    [[nodiscard]] static nlohmann::json discover_bot(
        const std::filesystem::path& manager_data_root);
    [[nodiscard]] static nlohmann::json probe_bot(
        const std::filesystem::path& manager_data_root,
        const ProcessSupervisor& process_supervisor);
    [[nodiscard]] static nlohmann::json probe_bot(
        const std::filesystem::path& manager_data_root,
        const BotCommandRunner& command_runner);
    [[nodiscard]] static nlohmann::json control_bot(
        const std::filesystem::path& manager_data_root,
        const ProcessSupervisor& process_supervisor,
        const std::string& action);
    [[nodiscard]] static nlohmann::json control_bot(
        const std::filesystem::path& manager_data_root,
        const BotCommandRunner& command_runner,
        const std::string& action);
};

}  // namespace arena::application
