#pragma once

#include <filesystem>
#include <nlohmann/json.hpp>
#include <vector>

namespace arena::application {

class EngineOnboarding final {
  public:
    [[nodiscard]] static nlohmann::json discover(
        const std::filesystem::path& manager_data_root,
        const std::vector<std::filesystem::path>& explicit_roots = {});
};

}  // namespace arena::application
