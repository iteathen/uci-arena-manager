#pragma once

#include "arena/domain/types.hpp"

#include <cstddef>
#include <cstdint>
#include <map>
#include <span>
#include <stdexcept>
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
    static void validate_wire_line(const std::string_view line) {
        if (line.size() > uci_metadata_maximum_line_bytes) {
            throw std::runtime_error("UCI wire line exceeds its byte bound");
        }

        std::size_t cursor = 0;
        while (cursor < line.size()) {
            const auto first = static_cast<std::uint8_t>(line[cursor]);
            std::uint32_t scalar = 0;
            std::size_t length = 0;
            if (first <= 0x7fU) {
                scalar = first;
                length = 1;
            } else if (first >= 0xc2U && first <= 0xdfU) {
                scalar = first & 0x1fU;
                length = 2;
            } else if (first >= 0xe0U && first <= 0xefU) {
                scalar = first & 0x0fU;
                length = 3;
            } else if (first >= 0xf0U && first <= 0xf4U) {
                scalar = first & 0x07U;
                length = 4;
            } else {
                throw std::runtime_error("UCI wire line contains invalid UTF-8");
            }
            if (cursor + length > line.size()) {
                throw std::runtime_error("UCI wire line contains truncated UTF-8");
            }
            for (std::size_t index = 1; index < length; ++index) {
                const auto continuation =
                    static_cast<std::uint8_t>(line[cursor + index]);
                if ((continuation & 0xc0U) != 0x80U) {
                    throw std::runtime_error(
                        "UCI wire line contains invalid UTF-8 continuation");
                }
                scalar = (scalar << 6U) | (continuation & 0x3fU);
            }
            if ((length == 3 && first == 0xe0U
                 && static_cast<std::uint8_t>(line[cursor + 1]) < 0xa0U)
                || (length == 3 && first == 0xedU
                    && static_cast<std::uint8_t>(line[cursor + 1]) >= 0xa0U)
                || (length == 4 && first == 0xf0U
                    && static_cast<std::uint8_t>(line[cursor + 1]) < 0x90U)
                || (length == 4 && first == 0xf4U
                    && static_cast<std::uint8_t>(line[cursor + 1]) >= 0x90U)
                || scalar > 0x10ffffU
                || (scalar >= 0xd800U && scalar <= 0xdfffU)) {
                throw std::runtime_error(
                    "UCI wire line contains non-scalar or overlong UTF-8");
            }
            if (scalar <= 0x1fU || scalar == 0x7fU
                || (scalar >= 0x80U && scalar <= 0x9fU)
                || scalar == 0x2028U || scalar == 0x2029U) {
                throw std::runtime_error(
                    "UCI wire line contains forbidden control characters");
            }
            cursor += length;
        }
    }

    [[nodiscard]] static UciMetadata parse(
        std::span<const std::string_view> lines);

    [[nodiscard]] static CanonicalUciOptions canonicalize_overrides(
        std::span<const domain::UciOption> advertised_options,
        const std::map<std::string, std::string>& option_overrides);
};

} // namespace arena::adapters
