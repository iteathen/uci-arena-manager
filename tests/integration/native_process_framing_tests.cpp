#include "arena/ports/native_process.hpp"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <filesystem>
#include <string>

#ifndef ARENA_MOCK_UCI_PATH
#error ARENA_MOCK_UCI_PATH must name the mock UCI engine
#endif

namespace {

using arena::ports::NativeProcess;
using arena::ports::NativeProcessIoLimits;

NativeProcessIoLimits small_limits() {
    return NativeProcessIoLimits{
        .maximum_input_line_bytes = 64,
        .maximum_output_line_bytes = 64,
        .maximum_queued_lines = 2,
        .maximum_queued_bytes = 128,
        .droppable_output_prefix = "info ",
    };
}

TEST_CASE("native process never silently evicts bounded control output") {
    const std::filesystem::path executable = ARENA_MOCK_UCI_PATH;
    NativeProcess process;
    process.start(executable, executable.parent_path(), {}, small_limits());
    process.write_line("framing-control-burst");
    REQUIRE_THROWS_WITH(
        process.read_line(std::chrono::seconds(2)),
        "process control output queue exceeds bound");
    process.stop(std::chrono::milliseconds(250));
}

TEST_CASE("native process drops only observations under queue pressure") {
    const std::filesystem::path executable = ARENA_MOCK_UCI_PATH;
    NativeProcess process;
    process.start(executable, executable.parent_path(), {}, small_limits());
    process.write_line("framing-info-burst");

    bool found_done = false;
    for (int attempt = 0; attempt < 8 && !found_done; ++attempt) {
        const auto line = process.read_line(std::chrono::seconds(2));
        REQUIRE(line.has_value());
        found_done = *line == "done";
    }
    REQUIRE(found_done);
    REQUIRE(process.dropped_observational_lines() > 0);
    process.stop(std::chrono::milliseconds(250));
}

TEST_CASE("native process drops an oversized observation and preserves later control") {
    const std::filesystem::path executable = ARENA_MOCK_UCI_PATH;
    NativeProcess process;
    process.start(executable, executable.parent_path(), {}, small_limits());
    process.write_line("framing-oversized-info");
    const auto line = process.read_line(std::chrono::seconds(2));
    REQUIRE(line == "done");
    REQUIRE(process.dropped_observational_lines() == 1);
    process.stop(std::chrono::milliseconds(250));
}

TEST_CASE("native process rejects invalid limits before launch") {
    const std::filesystem::path executable = ARENA_MOCK_UCI_PATH;
    NativeProcess process;

    auto limits = small_limits();
    limits.maximum_output_line_bytes = 0;
    REQUIRE_THROWS_WITH(
        process.start(executable, executable.parent_path(), {}, limits),
        "native process output line bound is invalid");
    REQUIRE_FALSE(process.running());

    limits = small_limits();
    limits.maximum_queued_bytes = limits.maximum_output_line_bytes - 1;
    REQUIRE_THROWS_WITH(
        process.start(executable, executable.parent_path(), {}, limits),
        "native process queued byte bound is invalid");
    REQUIRE_FALSE(process.running());
}

TEST_CASE("native process rejects oversized and ambiguously framed input") {
    const std::filesystem::path executable = ARENA_MOCK_UCI_PATH;
    NativeProcess process;
    process.start(executable, executable.parent_path(), {}, small_limits());

    REQUIRE_THROWS_WITH(
        process.write_line(std::string(65, 'x')),
        "process input line exceeds byte bound");
    REQUIRE_THROWS_WITH(
        process.write_line("uci\nquit"),
        "process input line contains a framing character");
    REQUIRE_THROWS_WITH(
        process.write_line(std::string("uci\0quit", 8)),
        "process input line contains a framing character");

    process.stop(std::chrono::milliseconds(250));
}

} // namespace
