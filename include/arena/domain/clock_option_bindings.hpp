#pragma once

#include "arena/domain/types.hpp"
#include <map>
#include <optional>
#include <string>

namespace arena::domain {
enum class ClockOptionField { initial_ms, increment_ms };
using ClockOptionBindings = std::map<std::string, ClockOptionField>;

[[nodiscard]] std::map<std::string, std::string> bind_clock_options(
    const std::map<std::string, std::string>& baseline,
    const ClockOptionBindings& bindings,
    const std::optional<TimeControl>& actual);
}  // namespace arena::domain
