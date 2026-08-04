#pragma once

#include "arena/domain/types.hpp"

#include <cstddef>
#include <map>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace arena::adapters {

inline constexpr std::size_t uci_metadata_maximum_lines = 1'024;
inline constexpr std::size_t uci_metadata_maximum_line_bytes = 4'096;
inline constexpr std::size_t uci_identity_maximum_value_bytes = 256;
inline constexpr std::size_t uci_identity_maximum_cumulative_bytes = 512;
inline constexpr std::size_t uci_option_maximum_count = 256;
inline constexpr std::size_t uci_option_maximum_name_bytes = 256;
inline constexpr std::size_t uci_option_maximum_value_bytes = 1'024;
inline constexpr std::size_t uci_option_maximum_variants = 256;
inline constexpr std::size_t uci_option_maximum_cumulative_bytes = 262'144;
inline constexpr std::size_t uci_override_maximum_count = 128;
inline constexpr std::size_t uci_override_maximum_cumulative_bytes = 65'536;

struct UciMetadata final {
    std::string engine_name;
    std::string engine_author;
    std::vector<domain::UciOption> options;
};

using CanonicalUciOptions = std::map<std::string, std::string, std::less<>>;

class UciOptionContract final {
public:
    static void validate_wire_line(std::string_view line);

    [[nodiscard]] static UciMetadata parse(
        std::span<const std::string_view> lines);

    [[nodiscard]] static CanonicalUciOptions canonicalize_overrides(
        std::span<const domain::UciOption> advertised_options,
        const std::map<std::string, std::string>& option_overrides);
};

} // namespace arena::adapters
