#include "arena/domain/clock_option_bindings.hpp"
#include <stdexcept>

namespace arena::domain {
std::map<std::string, std::string> bind_clock_options(
    const std::map<std::string, std::string>& baseline,
    const ClockOptionBindings& bindings,
    const std::optional<TimeControl>& actual) {
    auto effective = baseline;
    if (bindings.empty()) return effective;
    if (!actual) {
        throw std::invalid_argument("actual original clock control is required");
    }
    if (actual->initial_ms == 0 || actual->initial_ms > 9007199254740991ULL || actual->increment_ms > 9007199254740991ULL) {
        throw std::invalid_argument("actual original clock control is unknown");
    }
    for (const auto& [name, source] : bindings) {
        if (!baseline.contains(name)) throw std::invalid_argument("clock option is absent from baseline");
        switch (source) {
            case ClockOptionField::initial_ms: effective[name] = std::to_string(actual->initial_ms); break;
            case ClockOptionField::increment_ms: effective[name] = std::to_string(actual->increment_ms); break;
            default: throw std::invalid_argument("unknown clock option source");
        }
    }
    return effective;
}
}  // namespace arena::domain
