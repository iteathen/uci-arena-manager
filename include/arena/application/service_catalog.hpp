#pragma once

#include <nlohmann/json.hpp>
#include <string>

namespace arena::application {

class ServiceCatalog final {
  public:
    [[nodiscard]] static nlohmann::json profiles();
    [[nodiscard]] static nlohmann::json discover(const std::string& profile_id);
    [[nodiscard]] static nlohmann::json normalize_registration(
        const nlohmann::json& parameters);
};

}  // namespace arena::application
