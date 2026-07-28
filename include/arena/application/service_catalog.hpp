#pragma once

#include <filesystem>
#include <nlohmann/json.hpp>
#include <string>

namespace arena::application {

class ServiceCatalog final {
  public:
    [[nodiscard]] static nlohmann::json profiles();
    [[nodiscard]] static nlohmann::json discover(
        const std::string& profile_id,
        const std::filesystem::path& manager_data_root = {});
    [[nodiscard]] static nlohmann::json normalize_registration(
        const nlohmann::json& parameters);
    [[nodiscard]] static nlohmann::json installed_registrations(
        const std::filesystem::path& manager_data_root);
};

}  // namespace arena::application
