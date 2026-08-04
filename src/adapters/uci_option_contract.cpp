#include "arena/adapters/uci_option_contract.hpp"

#include <algorithm>
#include <charconv>
#include <cstdint>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace arena::adapters {
namespace {

struct Token final {
    std::string_view value;
    std::size_t begin{};
    std::size_t end{};
};

[[noreturn]] void reject(const std::string_view message) {
    throw std::runtime_error(std::string(message));
}

bool forbidden_scalar(const std::uint32_t scalar) noexcept {
    return scalar <= 0x1fU
        || scalar == 0x7fU
        || (scalar >= 0x80U && scalar <= 0x9fU)
        || scalar == 0x2028U
        || scalar == 0x2029U;
}

void require_text(
    const std::string_view value,
    const bool allow_empty,
    const std::size_t maximum_bytes,
    const std::string_view empty_error,
    const std::string_view size_error) {
    if (!allow_empty && value.empty()) {
        reject(empty_error);
    }
    if (value.size() > maximum_bytes) {
        reject(size_error);
    }

    std::size_t cursor = 0;
    while (cursor < value.size()) {
        const auto first = static_cast<std::uint8_t>(value[cursor]);
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
            reject("UCI metadata contains invalid UTF-8");
        }
        if (cursor + length > value.size()) {
            reject("UCI metadata contains truncated UTF-8");
        }
        for (std::size_t index = 1; index < length; ++index) {
            const auto continuation = static_cast<std::uint8_t>(value[cursor + index]);
            if ((continuation & 0xc0U) != 0x80U) {
                reject("UCI metadata contains invalid UTF-8 continuation");
            }
            scalar = (scalar << 6U) | (continuation & 0x3fU);
        }
        if ((length == 3 && first == 0xe0U
             && static_cast<std::uint8_t>(value[cursor + 1]) < 0xa0U)
            || (length == 3 && first == 0xedU
                && static_cast<std::uint8_t>(value[cursor + 1]) >= 0xa0U)
            || (length == 4 && first == 0xf0U
                && static_cast<std::uint8_t>(value[cursor + 1]) < 0x90U)
            || (length == 4 && first == 0xf4U
                && static_cast<std::uint8_t>(value[cursor + 1]) >= 0x90U)
            || scalar > 0x10ffffU
            || (scalar >= 0xd800U && scalar <= 0xdfffU)) {
            reject("UCI metadata contains non-scalar or overlong UTF-8");
        }
        if (forbidden_scalar(scalar)) {
            reject("UCI metadata contains forbidden control characters");
        }
        cursor += length;
    }
}

void require_no_edge_space(
    const std::string_view value,
    const std::string_view error) {
    if (!value.empty() && (value.front() == ' ' || value.back() == ' ')) {
        reject(error);
    }
}

std::string ascii_fold(const std::string_view value) {
    std::string folded(value);
    for (char& character : folded) {
        if (character >= 'A' && character <= 'Z') {
            character = static_cast<char>(character - 'A' + 'a');
        }
    }
    return folded;
}

std::vector<Token> tokenize(const std::string_view line) {
    std::vector<Token> result;
    result.reserve(16);
    std::size_t cursor = 0;
    while (cursor < line.size()) {
        while (cursor < line.size() && line[cursor] == ' ') {
            ++cursor;
        }
        if (cursor == line.size()) {
            break;
        }
        const std::size_t begin = cursor;
        while (cursor < line.size() && line[cursor] != ' ') {
            ++cursor;
        }
        result.push_back(Token{line.substr(begin, cursor - begin), begin, cursor});
    }
    return result;
}

std::string span_text(
    const std::string_view line,
    const std::vector<Token>& tokens,
    const std::size_t first,
    const std::size_t last_exclusive) {
    if (first >= last_exclusive || last_exclusive > tokens.size()) {
        return {};
    }
    return std::string(line.substr(
        tokens[first].begin,
        tokens[last_exclusive - 1].end - tokens[first].begin));
}

std::int64_t strict_integer(const std::string_view value) {
    if (value.empty()) {
        reject("UCI spin integer is empty");
    }
    std::size_t cursor = value.front() == '-' ? 1 : 0;
    if (cursor == value.size()) {
        reject("UCI spin integer syntax is invalid");
    }
    for (; cursor < value.size(); ++cursor) {
        if (value[cursor] < '0' || value[cursor] > '9') {
            reject("UCI spin integer syntax is invalid");
        }
    }
    std::int64_t result = 0;
    const auto parsed = std::from_chars(
        value.data(),
        value.data() + value.size(),
        result);
    if (parsed.ec != std::errc{}
        || parsed.ptr != value.data() + value.size()) {
        reject("UCI spin integer is outside int64 range");
    }
    return result;
}

void validate_option_record(const domain::UciOption& option) {
    require_text(
        option.name,
        false,
        uci_option_maximum_name_bytes,
        "UCI option name is empty",
        "UCI option name exceeds its byte bound");
    require_no_edge_space(option.name, "UCI option name has ambiguous edge spaces");

    if (option.type == "button") {
        if (!option.default_value.empty() || option.minimum || option.maximum
            || !option.variants.empty()) {
            reject("UCI button option has forbidden fields");
        }
        return;
    }
    if (option.type == "check") {
        if ((option.default_value != "true" && option.default_value != "false")
            || option.minimum || option.maximum || !option.variants.empty()) {
            reject("UCI check option contract is invalid");
        }
        return;
    }
    if (option.type == "spin") {
        if (!option.minimum || !option.maximum || !option.variants.empty()) {
            reject("UCI spin option is missing required fields");
        }
        const std::int64_t default_value = strict_integer(option.default_value);
        if (*option.minimum > default_value || default_value > *option.maximum) {
            reject("UCI spin default is outside advertised bounds");
        }
        return;
    }
    if (option.type == "combo") {
        if (option.minimum || option.maximum || option.variants.empty()
            || option.variants.size() > uci_option_maximum_variants) {
            reject("UCI combo option contract is invalid");
        }
        require_text(
            option.default_value,
            false,
            uci_option_maximum_value_bytes,
            "UCI combo default is empty",
            "UCI combo default exceeds its byte bound");
        std::set<std::string, std::less<>> variants;
        bool default_found = false;
        for (const std::string& variant : option.variants) {
            require_text(
                variant,
                false,
                uci_option_maximum_value_bytes,
                "UCI combo variant is empty",
                "UCI combo variant exceeds its byte bound");
            if (!variants.insert(variant).second) {
                reject("UCI combo variant is duplicated");
            }
            default_found = default_found || variant == option.default_value;
        }
        if (!default_found) {
            reject("UCI combo default is not an advertised variant");
        }
        return;
    }
    if (option.type == "string") {
        if (option.minimum || option.maximum || !option.variants.empty()) {
            reject("UCI string option has forbidden fields");
        }
        require_text(
            option.default_value,
            true,
            uci_option_maximum_value_bytes,
            "",
            "UCI string default exceeds its byte bound");
        return;
    }
    reject("UCI option type is unsupported");
}

domain::UciOption parse_button(
    domain::UciOption option,
    const std::vector<Token>& tokens,
    const std::size_t cursor) {
    if (cursor != tokens.size()) {
        reject("UCI button option has trailing fields");
    }
    return option;
}

domain::UciOption parse_check(
    domain::UciOption option,
    const std::vector<Token>& tokens,
    const std::size_t cursor) {
    if (tokens.size() != cursor + 2 || tokens[cursor].value != "default") {
        reject("UCI check option requires exactly one default field");
    }
    option.default_value = std::string(tokens[cursor + 1].value);
    validate_option_record(option);
    return option;
}

domain::UciOption parse_spin(
    domain::UciOption option,
    const std::vector<Token>& tokens,
    std::size_t cursor) {
    bool saw_default = false;
    bool saw_minimum = false;
    bool saw_maximum = false;
    while (cursor < tokens.size()) {
        if (cursor + 1 >= tokens.size()) {
            reject("UCI spin option has a field without a value");
        }
        const std::string_view field = tokens[cursor].value;
        const std::string_view value = tokens[cursor + 1].value;
        if (field == "default") {
            if (saw_default) {
                reject("UCI spin default field is repeated");
            }
            option.default_value = std::string(value);
            saw_default = true;
        } else if (field == "min") {
            if (saw_minimum) {
                reject("UCI spin min field is repeated");
            }
            option.minimum = strict_integer(value);
            saw_minimum = true;
        } else if (field == "max") {
            if (saw_maximum) {
                reject("UCI spin max field is repeated");
            }
            option.maximum = strict_integer(value);
            saw_maximum = true;
        } else {
            reject("UCI spin option contains an unknown field");
        }
        cursor += 2;
    }
    if (!saw_default || !saw_minimum || !saw_maximum) {
        reject("UCI spin option is missing required fields");
    }
    validate_option_record(option);
    return option;
}

domain::UciOption parse_combo(
    domain::UciOption option,
    const std::string_view line,
    const std::vector<Token>& tokens,
    const std::size_t cursor) {
    if (cursor >= tokens.size() || tokens[cursor].value != "default") {
        reject("UCI combo option must begin with one default field");
    }
    std::size_t first_variant = cursor + 1;
    while (first_variant < tokens.size()
           && tokens[first_variant].value != "var") {
        if (tokens[first_variant].value == "default") {
            reject("UCI combo default field is repeated");
        }
        ++first_variant;
    }
    option.default_value = span_text(line, tokens, cursor + 1, first_variant);
    if (first_variant == tokens.size()) {
        reject("UCI combo option has no variants");
    }

    std::size_t marker = first_variant;
    while (marker < tokens.size()) {
        if (tokens[marker].value != "var") {
            reject("UCI combo option contains trailing ambiguity");
        }
        const std::size_t value_begin = marker + 1;
        std::size_t next_marker = value_begin;
        while (next_marker < tokens.size()
               && tokens[next_marker].value != "var") {
            if (tokens[next_marker].value == "default") {
                reject("UCI combo default field is repeated");
            }
            ++next_marker;
        }
        option.variants.push_back(span_text(
            line,
            tokens,
            value_begin,
            next_marker));
        if (option.variants.size() > uci_option_maximum_variants) {
            reject("UCI combo option exceeds its variant-count bound");
        }
        marker = next_marker;
    }
    validate_option_record(option);
    return option;
}

domain::UciOption parse_string(
    domain::UciOption option,
    const std::string_view line,
    const std::vector<Token>& tokens,
    const std::size_t cursor) {
    if (cursor >= tokens.size() || tokens[cursor].value != "default") {
        reject("UCI string option requires one default field");
    }
    if (cursor + 1 < tokens.size()) {
        option.default_value = std::string(line.substr(tokens[cursor + 1].begin));
    }
    validate_option_record(option);
    return option;
}

domain::UciOption parse_option_line(const std::string_view line) {
    const std::vector<Token> tokens = tokenize(line);
    if (tokens.size() < 4 || tokens[0].value != "option"
        || tokens[1].value != "name") {
        reject("UCI option line has an invalid prefix");
    }

    std::size_t type_marker = 2;
    while (type_marker < tokens.size()
           && tokens[type_marker].value != "type") {
        ++type_marker;
    }
    if (type_marker == 2 || type_marker + 1 >= tokens.size()) {
        reject("UCI option line is missing name or type");
    }

    domain::UciOption option;
    option.name = span_text(line, tokens, 2, type_marker);
    option.type = std::string(tokens[type_marker + 1].value);
    require_text(
        option.name,
        false,
        uci_option_maximum_name_bytes,
        "UCI option name is empty",
        "UCI option name exceeds its byte bound");
    require_no_edge_space(option.name, "UCI option name has ambiguous edge spaces");

    const std::size_t fields = type_marker + 2;
    if (option.type == "button") {
        option = parse_button(std::move(option), tokens, fields);
    } else if (option.type == "check") {
        option = parse_check(std::move(option), tokens, fields);
    } else if (option.type == "spin") {
        option = parse_spin(std::move(option), tokens, fields);
    } else if (option.type == "combo") {
        option = parse_combo(std::move(option), line, tokens, fields);
    } else if (option.type == "string") {
        option = parse_string(std::move(option), line, tokens, fields);
    } else {
        reject("UCI option type is unsupported");
    }
    validate_option_record(option);
    return option;
}

} // namespace

UciMetadata UciOptionContract::parse(
    const std::span<const std::string_view> lines) {
    if (lines.size() > uci_metadata_maximum_lines) {
        reject("UCI metadata exceeds its line-count bound");
    }

    UciMetadata result;
    bool saw_name = false;
    bool saw_author = false;
    std::size_t identity_bytes = 0;
    std::size_t option_bytes = 0;
    std::set<std::string, std::less<>> folded_option_names;

    for (const std::string_view line : lines) {
        require_text(
            line,
            true,
            uci_metadata_maximum_line_bytes,
            "",
            "UCI metadata line exceeds its byte bound");
        require_no_edge_space(line, "UCI metadata line has ambiguous edge spaces");

        if (line.starts_with("id name")) {
            if (!line.starts_with("id name ")) {
                reject("UCI id name line is malformed");
            }
            if (saw_name) {
                reject("UCI id name line is duplicated");
            }
            const std::string_view value = line.substr(8);
            require_text(
                value,
                false,
                uci_identity_maximum_value_bytes,
                "UCI engine name is empty",
                "UCI engine name exceeds its byte bound");
            require_no_edge_space(value, "UCI engine name has ambiguous edge spaces");
            identity_bytes += value.size();
            if (identity_bytes > uci_identity_maximum_cumulative_bytes) {
                reject("UCI identity exceeds its cumulative byte bound");
            }
            result.engine_name = std::string(value);
            saw_name = true;
            continue;
        }
        if (line.starts_with("id author")) {
            if (!line.starts_with("id author ")) {
                reject("UCI id author line is malformed");
            }
            if (saw_author) {
                reject("UCI id author line is duplicated");
            }
            const std::string_view value = line.substr(10);
            require_text(
                value,
                false,
                uci_identity_maximum_value_bytes,
                "UCI engine author is empty",
                "UCI engine author exceeds its byte bound");
            require_no_edge_space(value, "UCI engine author has ambiguous edge spaces");
            identity_bytes += value.size();
            if (identity_bytes > uci_identity_maximum_cumulative_bytes) {
                reject("UCI identity exceeds its cumulative byte bound");
            }
            result.engine_author = std::string(value);
            saw_author = true;
            continue;
        }
        if (line.starts_with("id ") || line == "id") {
            reject("UCI identity line is unsupported or malformed");
        }
        if (line.starts_with("option") || line == "option") {
            if (!line.starts_with("option name ")) {
                reject("UCI option line is malformed");
            }
            if (result.options.size() >= uci_option_maximum_count) {
                reject("UCI option count exceeds its bound");
            }
            option_bytes += line.size();
            if (option_bytes > uci_option_maximum_cumulative_bytes) {
                reject("UCI option metadata exceeds its cumulative byte bound");
            }
            domain::UciOption option = parse_option_line(line);
            const std::string folded = ascii_fold(option.name);
            if (!folded_option_names.insert(folded).second) {
                reject("UCI option name is duplicated under ASCII case folding");
            }
            result.options.push_back(std::move(option));
        }
    }

    if (!saw_name) {
        reject("UCI engine name is missing");
    }
    return result;
}

CanonicalUciOptions UciOptionContract::canonicalize_overrides(
    const std::span<const domain::UciOption> advertised_options,
    const std::map<std::string, std::string>& option_overrides) {
    if (advertised_options.size() > uci_option_maximum_count) {
        reject("advertised UCI option count exceeds its bound");
    }
    if (option_overrides.size() > uci_override_maximum_count) {
        reject("UCI override count exceeds its bound");
    }

    std::map<std::string, const domain::UciOption*, std::less<>> advertised;
    for (const domain::UciOption& option : advertised_options) {
        validate_option_record(option);
        const std::string folded = ascii_fold(option.name);
        if (!advertised.emplace(folded, &option).second) {
            reject("advertised UCI option name is duplicated under ASCII case folding");
        }
    }

    CanonicalUciOptions canonical;
    std::set<std::string, std::less<>> folded_overrides;
    std::size_t cumulative_bytes = 0;
    for (const auto& [requested_name, requested_value] : option_overrides) {
        require_text(
            requested_name,
            false,
            uci_option_maximum_name_bytes,
            "UCI override name is empty",
            "UCI override name exceeds its byte bound");
        require_no_edge_space(requested_name, "UCI override name has ambiguous edge spaces");
        require_text(
            requested_value,
            true,
            uci_option_maximum_value_bytes,
            "",
            "UCI override value exceeds its byte bound");
        cumulative_bytes += requested_name.size() + requested_value.size();
        if (cumulative_bytes > uci_override_maximum_cumulative_bytes) {
            reject("UCI overrides exceed their cumulative byte bound");
        }

        const std::string folded = ascii_fold(requested_name);
        if (!folded_overrides.insert(folded).second) {
            reject("UCI override name is duplicated under ASCII case folding");
        }
        const auto found = advertised.find(folded);
        if (found == advertised.end()) {
            reject("UCI override names an unadvertised option");
        }
        const domain::UciOption& option = *found->second;
        if (option.type == "button") {
            reject("UCI button option cannot be persisted as a value");
        }
        if (option.type == "check"
            && requested_value != "true" && requested_value != "false") {
            reject("UCI check override must be true or false");
        }
        if (option.type == "spin") {
            const std::int64_t value = strict_integer(requested_value);
            if (!option.minimum || !option.maximum
                || value < *option.minimum || value > *option.maximum) {
                reject("UCI spin override is outside advertised bounds");
            }
        }
        if (option.type == "combo"
            && std::find(
                option.variants.begin(),
                option.variants.end(),
                requested_value) == option.variants.end()) {
            reject("UCI combo override is not an advertised variant");
        }
        canonical.emplace(option.name, requested_value);
    }
    return canonical;
}

} // namespace arena::adapters
