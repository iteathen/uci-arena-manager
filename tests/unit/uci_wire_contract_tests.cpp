#include "arena/adapters/uci_option_contract.hpp"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <string>
#include <string_view>

namespace {

using arena::adapters::UciOptionContract;
using arena::adapters::uci_metadata_maximum_line_bytes;

TEST_CASE("UCI wire admission accepts exact bounded UTF-8 scalar text") {
    UciOptionContract::validate_wire_line("");
    UciOptionContract::validate_wire_line("id name Exact Engine");
    UciOptionContract::validate_wire_line("info string caf\xC3\xA9");
    UciOptionContract::validate_wire_line(
        std::string(uci_metadata_maximum_line_bytes, 'x'));
}

TEST_CASE("UCI wire admission rejects byte overflow") {
    REQUIRE_THROWS_WITH(
        UciOptionContract::validate_wire_line(
            std::string(uci_metadata_maximum_line_bytes + 1, 'x')),
        "UCI wire line exceeds its byte bound");
}

TEST_CASE("UCI wire admission rejects malformed and non-scalar UTF-8") {
    const std::array invalid{
        std::string("x\x80", 2),
        std::string("x\xC2", 2),
        std::string("x\xC3\x28", 3),
        std::string("x\xE0\x80\x80", 4),
        std::string("x\xED\xA0\x80", 4),
        std::string("x\xF0\x80\x80\x80", 5),
        std::string("x\xF4\x90\x80\x80", 5),
    };
    for (const std::string& value : invalid) {
        REQUIRE_THROWS(UciOptionContract::validate_wire_line(value));
    }
}

TEST_CASE("UCI wire admission rejects every forbidden control class") {
    const std::array forbidden{
        std::string("x\x00", 2),
        std::string("x\x01", 2),
        std::string("x\x1F", 2),
        std::string("x\x7F", 2),
        std::string("x\xC2\x80", 3),
        std::string("x\xC2\x9F", 3),
        std::string("x\xE2\x80\xA8", 4),
        std::string("x\xE2\x80\xA9", 4),
    };
    for (const std::string& value : forbidden) {
        REQUIRE_THROWS_WITH(
            UciOptionContract::validate_wire_line(value),
            "UCI wire line contains forbidden control characters");
    }
}

} // namespace
