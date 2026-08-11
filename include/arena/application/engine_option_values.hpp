#pragma once

#include <map>
#include <nlohmann/json_fwd.hpp>
#include <string>

namespace arena::application {

using EngineOptionValues = std::map<std::string, std::string>;

[[nodiscard]] EngineOptionValues normalize_engine_option_values(
    const nlohmann::json& value);

[[nodiscard]] nlohmann::json engine_option_values_json(
    const EngineOptionValues& values);

}  // namespace arena::application
