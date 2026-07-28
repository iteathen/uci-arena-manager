#include "arena/adapters/uci_process.hpp"

#include "arena/ports/native_process.hpp"

#include <sodium.h>

#include <array>
#include <charconv>
#include <chrono>
#include <fstream>
#include <iomanip>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string_view>

namespace arena::adapters {
namespace {

using Clock = std::chrono::steady_clock;

std::string fingerprint(const std::filesystem::path& executable) {
    if (sodium_init() < 0) {
        throw std::runtime_error("libsodium initialization failed");
    }
    std::ifstream input(executable, std::ios::binary);
    if (!input) {
        throw std::runtime_error("engine executable cannot be read");
    }
    crypto_hash_sha256_state state{};
    crypto_hash_sha256_init(&state);
    std::array<unsigned char, 64 * 1024> buffer{};
    while (input) {
        input.read(reinterpret_cast<char*>(buffer.data()), buffer.size());
        const auto count = input.gcount();
        if (count > 0) {
            crypto_hash_sha256_update(
                &state, buffer.data(), static_cast<unsigned long long>(count));
        }
    }
    std::array<unsigned char, crypto_hash_sha256_BYTES> digest{};
    crypto_hash_sha256_final(&state, digest.data());
    std::array<char, crypto_hash_sha256_BYTES * 2 + 1> encoded{};
    sodium_bin2hex(encoded.data(), encoded.size(), digest.data(), digest.size());
    return encoded.data();
}

std::vector<std::string> split(const std::string& value) {
    std::istringstream input(value);
    std::vector<std::string> result;
    for (std::string token; input >> token;) {
        result.push_back(std::move(token));
    }
    return result;
}

std::int64_t parse_integer(const std::string& value, const std::string& field) {
    std::int64_t result = 0;
    const auto parsed =
        std::from_chars(value.data(), value.data() + value.size(), result);
    if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size()) {
        throw std::runtime_error("UCI option has an invalid " + field + " value");
    }
    return result;
}

domain::UciOption parse_option(const std::string& line) {
    const auto tokens = split(line);
    domain::UciOption option;
    if (tokens.size() < 5 || tokens[0] != "option" || tokens[1] != "name") {
        return option;
    }
    std::size_t cursor = 2;
    while (cursor < tokens.size() && tokens[cursor] != "type") {
        if (!option.name.empty()) {
            option.name += ' ';
        }
        option.name += tokens[cursor++];
    }
    if (cursor + 1 >= tokens.size()) {
        option.name.clear();
        return option;
    }
    option.type = tokens[++cursor];
    ++cursor;
    while (cursor < tokens.size()) {
        const std::string field = tokens[cursor++];
        if (field == "default") {
            while (cursor < tokens.size() && tokens[cursor] != "min" &&
                   tokens[cursor] != "max" && tokens[cursor] != "var") {
                if (!option.default_value.empty()) {
                    option.default_value += ' ';
                }
                option.default_value += tokens[cursor++];
            }
        } else if (field == "min" && cursor < tokens.size()) {
            option.minimum = parse_integer(tokens[cursor++], "min");
        } else if (field == "max" && cursor < tokens.size()) {
            option.maximum = parse_integer(tokens[cursor++], "max");
        } else if (field == "var") {
            std::string variant;
            while (cursor < tokens.size() && tokens[cursor] != "var") {
                if (!variant.empty()) {
                    variant += ' ';
                }
                variant += tokens[cursor++];
            }
            option.variants.push_back(std::move(variant));
        }
    }
    return option;
}

std::string value_after(const std::string& line, const std::string_view prefix) {
    return line.starts_with(prefix) ? line.substr(prefix.size()) : std::string{};
}

std::string await_token(
    ports::NativeProcess& process,
    const std::string_view token,
    const Clock::time_point deadline,
    UciProbeResult& result,
    const bool capture_metadata) {
    while (Clock::now() < deadline) {
        const auto remaining =
            std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now());
        const auto line = process.read_line(std::min(remaining, std::chrono::milliseconds(250)));
        if (!line) {
            if (!process.running()) {
                throw std::runtime_error("engine exited before " + std::string(token));
            }
            continue;
        }
        if (capture_metadata) {
            if (line->starts_with("id name ")) {
                result.engine_name = value_after(*line, "id name ");
            } else if (line->starts_with("id author ")) {
                result.engine_author = value_after(*line, "id author ");
            } else if (line->starts_with("option ")) {
                auto option = parse_option(*line);
                if (!option.name.empty() && !option.type.empty()) {
                    result.options.push_back(std::move(option));
                }
            }
        }
        if (*line == token || line->starts_with(std::string(token) + " ")) {
            return *line;
        }
    }
    throw std::runtime_error("engine timed out waiting for " + std::string(token));
}

bool legal_starting_bestmove(const std::string& move) {
    static const std::set<std::string, std::less<>> legal = {
        "a2a3", "a2a4", "b2b3", "b2b4", "c2c3", "c2c4", "d2d3",
        "d2d4", "e2e3", "e2e4", "f2f3", "f2f4", "g2g3", "g2g4",
        "h2h3", "h2h4", "b1a3", "b1c3", "g1f3", "g1h3",
    };
    return legal.contains(move);
}

}  // namespace

UciProbeResult UciProcess::probe(
    const std::filesystem::path& executable,
    const std::filesystem::path& working_directory,
    const std::vector<std::string>& arguments,
    std::chrono::milliseconds timeout) {
    UciProbeResult result;
    const auto started = Clock::now();
    ports::NativeProcess process;
    try {
        result.fingerprint = fingerprint(executable);
        process.start(executable, working_directory, arguments);
        const auto deadline = Clock::now() + timeout;

        process.write_line("uci");
        await_token(process, "uciok", deadline, result, true);
        process.write_line("isready");
        await_token(process, "readyok", deadline, result, false);
        process.write_line("ucinewgame");
        process.write_line("position startpos");
        process.write_line("go depth 1");
        const std::string bestmove_line =
            await_token(process, "bestmove", deadline, result, false);
        const auto parts = split(bestmove_line);
        if (parts.size() < 2 || !legal_starting_bestmove(parts[1])) {
            throw std::runtime_error("engine returned an illegal starting-position bestmove");
        }
        result.bestmove = parts[1];
        process.stop(std::chrono::seconds(2));
        result.ok = true;
    } catch (const std::exception& error) {
        result.error = error.what();
        try {
            process.stop(std::chrono::milliseconds(250));
        } catch (...) {
        }
    }
    result.elapsed_ms = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - started).count());
    result.dropped_info_lines = process.dropped_observational_lines();
    return result;
}

}  // namespace arena::adapters
