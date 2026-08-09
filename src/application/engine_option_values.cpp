#include "arena/application/engine_option_values.hpp"

#include <algorithm>
#include <cstdint>
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <utility>

namespace arena::application {

EngineOptionValues normalize_engine_option_values(const nlohmann::json& value) {
    if (!value.is_object() || value.size() > 128) {
        throw std::invalid_argument(
            "registered engine option overrides must be an object with at most 128 entries");
    }
    EngineOptionValues result;
    for (const auto& [name, raw] : value.items()) {
        if (name.empty() || name.size() > 128 ||
            std::ranges::any_of(name, [](const unsigned char character) {
                return character < 0x20 || character == 0x7f;
            })) {
            throw std::invalid_argument("registered UCI option name is invalid");
        }
        std::string text;
        if (raw.is_string()) {
            text = raw.get<std::string>();
        } else if (raw.is_boolean()) {
            text = raw.get<bool>() ? "true" : "false";
        } else if (raw.is_number_integer()) {
            text = std::to_string(raw.get<std::int64_t>());
        } else if (raw.is_number_unsigned()) {
            text = std::to_string(raw.get<std::uint64_t>());
        } else {
            throw std::invalid_argument(
                "registered UCI option values must be strings, booleans, or integers");
        }
        if (text.size() > 512 || text.find('\n') != std::string::npos ||
            text.find('\r') != std::string::npos ||
            text.find('\0') != std::string::npos) {
            throw std::invalid_argument("registered UCI option value is invalid");
        }
        result.emplace(name, std::move(text));
    }
    return result;
}

nlohmann::json engine_option_values_json(const EngineOptionValues& values) {
    nlohmann::json result = nlohmann::json::object();
    for (const auto& [name, value] : values) {
        result[name] = value;
    }
    return result;
}

}  // namespace arena::application
