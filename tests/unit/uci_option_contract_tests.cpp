#include "arena/adapters/uci_option_contract.hpp"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>
#include <limits>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using arena::adapters::CanonicalUciOptions;
using arena::adapters::UciOptionContract;
using arena::adapters::uci_metadata_maximum_lines;
using arena::adapters::uci_option_maximum_count;
using arena::adapters::uci_option_maximum_value_bytes;
using arena::adapters::uci_override_maximum_count;
using arena::domain::UciOption;

std::vector<std::string_view> views(const std::vector<std::string>& lines) {
    std::vector<std::string_view> result;
    result.reserve(lines.size());
    for (const std::string& line : lines) {
        result.push_back(line);
    }
    return result;
}

TEST_CASE("UCI metadata admits every exact option type deterministically") {
    constexpr std::array lines{
        std::string_view{"id name Exact Engine"},
        std::string_view{"id author UCI Arena"},
        std::string_view{"option name Clear Hash type button"},
        std::string_view{"option name Ponder type check default false"},
        std::string_view{"option name Style type combo default Balanced var Fast var Balanced var Defensive"},
        std::string_view{"option name Hash type spin min 1 default 64 max 4096"},
        std::string_view{"option name Notes type string default"},
        std::string_view{"uciok"},
    };

    const auto metadata = UciOptionContract::parse(lines);
    REQUIRE(metadata.engine_name == "Exact Engine");
    REQUIRE(metadata.engine_author == "UCI Arena");
    REQUIRE(metadata.options.size() == 5);

    REQUIRE(metadata.options[0].name == "Clear Hash");
    REQUIRE(metadata.options[0].type == "button");
    REQUIRE(metadata.options[1].default_value == "false");
    REQUIRE(metadata.options[2].variants == std::vector<std::string>{
        "Fast", "Balanced", "Defensive"});
    REQUIRE(metadata.options[3].minimum == 1);
    REQUIRE(metadata.options[3].maximum == 4096);
    REQUIRE(metadata.options[3].default_value == "64");
    REQUIRE(metadata.options[4].type == "string");
    REQUIRE(metadata.options[4].default_value.empty());
}

TEST_CASE("UCI identity requires one name and rejects every duplicate") {
    constexpr std::array missing{
        std::string_view{"id author Author"},
        std::string_view{"uciok"},
    };
    REQUIRE_THROWS_WITH(
        UciOptionContract::parse(missing),
        "UCI engine name is missing");

    constexpr std::array duplicate_name{
        std::string_view{"id name Same"},
        std::string_view{"id name Same"},
    };
    REQUIRE_THROWS_WITH(
        UciOptionContract::parse(duplicate_name),
        "UCI id name line is duplicated");

    constexpr std::array duplicate_author{
        std::string_view{"id name Engine"},
        std::string_view{"id author Same"},
        std::string_view{"id author Same"},
    };
    REQUIRE_THROWS_WITH(
        UciOptionContract::parse(duplicate_author),
        "UCI id author line is duplicated");

    constexpr std::array empty_name{
        std::string_view{"id name "},
    };
    REQUIRE_THROWS(UciOptionContract::parse(empty_name));
}

TEST_CASE("UCI metadata rejects malformed UTF-8 and forbidden scalar classes") {
    const std::array forbidden{
        std::string("id name bad\x01", 12),
        std::string("id name bad\x7f", 12),
        std::string("id name bad\xc2\x80", 13),
        std::string("id name bad\xe2\x80\xa8", 14),
        std::string("id name bad\xe2\x80\xa9", 14),
        std::string("id name bad\xc3\x28", 13),
    };
    for (const std::string& line : forbidden) {
        const std::array<std::string_view, 1> input{line};
        REQUIRE_THROWS(UciOptionContract::parse(input));
    }
}

TEST_CASE("UCI metadata enforces line option and cumulative bounds") {
    std::vector<std::string> too_many_lines(uci_metadata_maximum_lines + 1);
    REQUIRE_THROWS_WITH(
        UciOptionContract::parse(views(too_many_lines)),
        "UCI metadata exceeds its line-count bound");

    std::vector<std::string> too_many_options;
    too_many_options.push_back("id name Engine");
    for (std::size_t index = 0; index <= uci_option_maximum_count; ++index) {
        too_many_options.push_back(
            "option name Option" + std::to_string(index) + " type button");
    }
    REQUIRE_THROWS_WITH(
        UciOptionContract::parse(views(too_many_options)),
        "UCI option count exceeds its bound");

    std::vector<std::string> too_many_bytes;
    too_many_bytes.push_back("id name Engine");
    for (std::size_t index = 0; index < uci_option_maximum_count; ++index) {
        too_many_bytes.push_back(
            "option name Option" + std::to_string(index)
            + " type string default "
            + std::string(uci_option_maximum_value_bytes, 'x'));
    }
    REQUIRE_THROWS_WITH(
        UciOptionContract::parse(views(too_many_bytes)),
        "UCI option metadata exceeds its cumulative byte bound");
}

TEST_CASE("UCI option names are unique under ASCII case folding") {
    constexpr std::array lines{
        std::string_view{"id name Engine"},
        std::string_view{"option name Hash Size type spin default 16 min 1 max 1024"},
        std::string_view{"option name hash size type spin default 16 min 1 max 1024"},
    };
    REQUIRE_THROWS_WITH(
        UciOptionContract::parse(lines),
        "UCI option name is duplicated under ASCII case folding");
}

TEST_CASE("UCI option type grammar rejects repeated unknown and forbidden fields") {
    const std::vector<std::string> invalid{
        "option name Button type button default x",
        "option name Check type check default true min 0",
        "option name Check type check default maybe",
        "option name Spin type spin default 1 default 2 min 0 max 3",
        "option name Spin type spin default 1 min 0 bogus 3 max 4",
        "option name Spin type spin default +1 min 0 max 3",
        "option name Spin type spin default 4 min 0 max 3",
        "option name Spin type spin default 9223372036854775808 min 0 max 3",
        "option name Combo type combo default A",
        "option name Combo type combo default A var A var A",
        "option name Combo type combo default A var",
        "option name Combo type combo default A var B",
        "option name String type string",
        "option name Unknown type path default x",
    };
    for (const std::string& option : invalid) {
        const std::array<std::string_view, 2> lines{
            "id name Engine",
            option,
        };
        REQUIRE_THROWS(UciOptionContract::parse(lines));
    }
}

TEST_CASE("UCI spin accepts exact int64 boundaries") {
    const std::string minimum = std::to_string(std::numeric_limits<std::int64_t>::min());
    const std::string maximum = std::to_string(std::numeric_limits<std::int64_t>::max());
    const std::string line = "option name Range type spin default 0 min "
        + minimum + " max " + maximum;
    const std::array<std::string_view, 2> lines{"id name Engine", line};
    const auto metadata = UciOptionContract::parse(lines);
    REQUIRE(metadata.options[0].minimum == std::numeric_limits<std::int64_t>::min());
    REQUIRE(metadata.options[0].maximum == std::numeric_limits<std::int64_t>::max());
}

TEST_CASE("UCI overrides resolve case-insensitively and retain advertised spelling") {
    const std::vector<UciOption> advertised{
        UciOption{"Hash", "spin", "64", 1, 4096, {}},
        UciOption{"Ponder", "check", "false", std::nullopt, std::nullopt, {}},
        UciOption{"Style", "combo", "Balanced", std::nullopt, std::nullopt,
            {"Fast", "Balanced"}},
        UciOption{"Notes", "string", "", std::nullopt, std::nullopt, {}},
    };
    const std::map<std::string, std::string> requested{
        {"hash", "128"},
        {"NOTES", ""},
        {"ponder", "true"},
        {"STYLE", "Fast"},
    };
    const CanonicalUciOptions canonical =
        UciOptionContract::canonicalize_overrides(advertised, requested);
    const CanonicalUciOptions expected{
        {"Hash", "128"},
        {"Notes", ""},
        {"Ponder", "true"},
        {"Style", "Fast"},
    };
    REQUIRE(canonical == expected);
}

TEST_CASE("advertised UCI option records obey the cumulative metadata bound") {
    std::vector<UciOption> advertised;
    advertised.reserve(uci_option_maximum_count);
    for (std::size_t index = 0; index < uci_option_maximum_count; ++index) {
        advertised.push_back(UciOption{
            "Text" + std::to_string(index),
            "string",
            std::string(uci_option_maximum_value_bytes, 'x'),
            std::nullopt,
            std::nullopt,
            {},
        });
    }
    REQUIRE_THROWS_WITH(
        UciOptionContract::canonicalize_overrides(advertised, {}),
        "advertised UCI option metadata exceeds its cumulative byte bound");
}

TEST_CASE("UCI overrides reject type violations and folded duplicates") {
    const std::vector<UciOption> advertised{
        UciOption{"Clear Hash", "button", "", std::nullopt, std::nullopt, {}},
        UciOption{"Hash", "spin", "64", 1, 4096, {}},
        UciOption{"Ponder", "check", "false", std::nullopt, std::nullopt, {}},
        UciOption{"Style", "combo", "Balanced", std::nullopt, std::nullopt,
            {"Fast", "Balanced"}},
    };
    for (const auto& invalid : std::vector<std::map<std::string, std::string>>{
        {{"Clear Hash", "press"}},
        {{"Hash", "0"}},
        {{"Hash", "+1"}},
        {{"Ponder", "TRUE"}},
        {{"Style", "Unknown"}},
        {{"Missing", "1"}},
        {{"Hash", "1"}, {"hash", "2"}},
    }) {
        REQUIRE_THROWS(UciOptionContract::canonicalize_overrides(advertised, invalid));
    }
}

TEST_CASE("UCI override cardinality and cumulative bytes are bounded") {
    std::vector<UciOption> advertised;
    std::map<std::string, std::string> requested;
    for (std::size_t index = 0; index <= uci_override_maximum_count; ++index) {
        const std::string name = "Text" + std::to_string(index);
        advertised.push_back(UciOption{
            name, "string", "", std::nullopt, std::nullopt, {}});
        requested.emplace(name, "x");
    }
    REQUIRE_THROWS_WITH(
        UciOptionContract::canonicalize_overrides(advertised, requested),
        "UCI override count exceeds its bound");

    advertised.clear();
    requested.clear();
    for (std::size_t index = 0; index < 65; ++index) {
        const std::string name = "Value" + std::to_string(index);
        advertised.push_back(UciOption{
            name, "string", "", std::nullopt, std::nullopt, {}});
        requested.emplace(name, std::string(uci_option_maximum_value_bytes, 'x'));
    }
    REQUIRE_THROWS_WITH(
        UciOptionContract::canonicalize_overrides(advertised, requested),
        "UCI overrides exceed their cumulative byte bound");
}

} // namespace
