#pragma once

#include <nlohmann/json.hpp>
#include <string>

namespace arena::adapters {

class ReportRenderer final {
  public:
    [[nodiscard]] static std::string json(const nlohmann::json& document);
    [[nodiscard]] static std::string csv(const nlohmann::json& document);
    [[nodiscard]] static std::string html(const nlohmann::json& document);
    [[nodiscard]] static std::string pgn(const nlohmann::json& document);
    [[nodiscard]] static std::string pdf(const nlohmann::json& document);
};

}  // namespace arena::adapters
