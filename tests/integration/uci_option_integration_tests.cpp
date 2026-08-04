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

TEST_CASE("UCI probe treats oversized control output as fatal") {
    const std::filesystem::path executable = ARENA_MOCK_UCI_PATH;
    const auto result = UciProcess::probe(
        executable,
        executable.parent_path(),
        {"--oversized-control-line"},
        std::chrono::seconds(5));
    REQUIRE_FALSE(result.ok);
    REQUIRE(result.error == "process control output line exceeds byte bound");
}

TEST_CASE("UCI probe may drop oversized observation output without losing control") {
    const std::filesystem::path executable = ARENA_MOCK_UCI_PATH;
    const auto result = UciProcess::probe(
        executable,
        executable.parent_path(),
        {"--oversized-info-line"},
        std::chrono::seconds(5));
    REQUIRE(result.ok);
    REQUIRE(result.bestmove == "e2e4");
    REQUIRE(result.dropped_info_lines >= 1);
}

TEST_CASE("UCI probe rejects forbidden wire control characters") {
    const std::filesystem::path executable = ARENA_MOCK_UCI_PATH;
    const auto result = UciProcess::probe(
        executable,
        executable.parent_path(),
        {"--invalid-wire-control"},
        std::chrono::seconds(5));
    REQUIRE_FALSE(result.ok);
    REQUIRE(result.error == "UCI wire line contains forbidden control characters");
}

TEST_CASE("UCI probe rejects ambiguous uciok and readyok tokens") {
    const std::filesystem::path executable = ARENA_MOCK_UCI_PATH;
    const auto uciok = UciProcess::probe(
        executable,
        executable.parent_path(),
        {"--ambiguous-uciok"},
        std::chrono::seconds(5));
    REQUIRE_FALSE(uciok.ok);
    REQUIRE(uciok.error == "engine exited before uciok");

    const auto readyok = UciProcess::probe(
        executable,
        executable.parent_path(),
        {"--ambiguous-readyok"},
        std::chrono::seconds(5));
    REQUIRE_FALSE(readyok.ok);
    REQUIRE(readyok.error == "engine exited before readyok");
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
