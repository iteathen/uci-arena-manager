#include "arena/adapters/uci_process.hpp"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <filesystem>
#include <string>
#include <vector>

#ifndef ARENA_MOCK_UCI_PATH
#error ARENA_MOCK_UCI_PATH must name the mock UCI engine
#endif

namespace {

using arena::adapters::UciProcess;

TEST_CASE("UCI probe rejects duplicate identity through the production contract") {
    const std::filesystem::path executable = ARENA_MOCK_UCI_PATH;
    const auto result = UciProcess::probe(
        executable,
        executable.parent_path(),
        {"--duplicate-id-name"},
        std::chrono::seconds(5));
    REQUIRE_FALSE(result.ok);
    REQUIRE(result.error == "UCI id name line is duplicated");
}

TEST_CASE("UCI probe rejects repeated option fields through the production contract") {
    const std::filesystem::path executable = ARENA_MOCK_UCI_PATH;
    const auto result = UciProcess::probe(
        executable,
        executable.parent_path(),
        {"--malformed-option"},
        std::chrono::seconds(5));
    REQUIRE_FALSE(result.ok);
    REQUIRE(result.error == "UCI spin min field is repeated");
}

TEST_CASE("UCI probe bounds metadata before accumulating the complete response") {
    const std::filesystem::path executable = ARENA_MOCK_UCI_PATH;
    const auto result = UciProcess::probe(
        executable,
        executable.parent_path(),
        {"--too-many-metadata-lines"},
        std::chrono::seconds(5));
    REQUIRE_FALSE(result.ok);
    REQUIRE(result.error == "UCI metadata exceeds its line-count bound");
}

TEST_CASE("UCI probe sends overrides using advertised canonical option spelling") {
    const std::filesystem::path executable = ARENA_MOCK_UCI_PATH;
    const auto result = UciProcess::probe(
        executable,
        executable.parent_path(),
        {"--require-style-risky"},
        std::chrono::seconds(5),
        {{"style", "Risky"}});
    REQUIRE(result.ok);
    REQUIRE(result.bestmove == "e2e4");
}

} // namespace
