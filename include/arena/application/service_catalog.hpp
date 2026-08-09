#pragma once

#include "arena/application/process_supervisor.hpp"

#include <chrono>
#include <filesystem>
#include <functional>
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <string>
#include <vector>

namespace arena::application {

class BotQualificationFailure final : public std::runtime_error {
  public:
    BotQualificationFailure(
        const std::string& code,
        const std::string& stage,
        const std::string& detail);
    [[nodiscard]] const std::string& code() const noexcept;
    [[nodiscard]] const std::string& stage() const noexcept;
    [[nodiscard]] const std::string& detail() const noexcept;

  private:
    std::string code_;
    std::string stage_;
    std::string detail_;
};

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
    [[nodiscard]] static nlohmann::json qualify_bot(
        const std::filesystem::path& manager_data_root,
        const ProcessSupervisor& process_supervisor);
    [[nodiscard]] static nlohmann::json qualify_bot(
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
    [[nodiscard]] static nlohmann::json bot_options(
        const std::filesystem::path& manager_data_root,
        const ProcessSupervisor& process_supervisor);
    [[nodiscard]] static nlohmann::json bot_options(
        const std::filesystem::path& manager_data_root,
        const BotCommandRunner& command_runner);
    [[nodiscard]] static nlohmann::json update_bot_options(
        const std::filesystem::path& manager_data_root,
        const ProcessSupervisor& process_supervisor,
        const nlohmann::json& settings,
        bool confirm_rated,
        bool confirm_outbound);
    [[nodiscard]] static nlohmann::json update_bot_options(
        const std::filesystem::path& manager_data_root,
        const BotCommandRunner& command_runner,
        const nlohmann::json& settings,
        bool confirm_rated,
        bool confirm_outbound);
    [[nodiscard]] static nlohmann::json control_bot_account(
        const std::filesystem::path& manager_data_root,
        const ProcessSupervisor& process_supervisor,
        const std::string& action,
        const nlohmann::json& parameters);
    [[nodiscard]] static nlohmann::json control_bot_account(
        const std::filesystem::path& manager_data_root,
        const BotCommandRunner& command_runner,
        const std::string& action,
        const nlohmann::json& parameters);
};

}  // namespace arena::application
