#include "arena/adapters/uci_process.hpp"

#include "arena/adapters/uci_option_contract.hpp"
#include "arena/ports/native_process.hpp"

#include <sodium.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <fstream>
#include <set>
#include <span>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <vector>

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

std::string ascii_fold(const std::string_view value) {
    std::string folded(value);
    for (char& character : folded) {
        if (character >= 'A' && character <= 'Z') {
            character = static_cast<char>(character - 'A' + 'a');
        }
    }
    return folded;
}

bool safe_token(const std::string_view value, const std::size_t maximum) {
    return !value.empty() && value.size() <= maximum &&
           std::all_of(value.begin(), value.end(), [](const unsigned char character) {
               return std::isalnum(character) != 0 || character == '_' ||
                      character == '-' || character == '.';
           });
}

bool decimal_fingerprint(const std::string_view value) {
    return !value.empty() && value.size() <= 20 &&
           std::all_of(value.begin(), value.end(), [](const unsigned char character) {
               return std::isdigit(character) != 0;
           });
}

void capture_runtime_model_identity(
    const std::string& line,
    UciProbeResult& result) {
    constexpr std::string_view prefix = "info string model loaded:";
    constexpr std::string_view fingerprint_marker = ":fingerprint:";
    constexpr std::string_view provider_marker = ":provider:";
    constexpr std::string_view provider_fingerprint_marker =
        ":provider_numeric_fingerprint:";
    if (!line.starts_with(prefix)) {
        return;
    }
    const auto fingerprint_begin = line.rfind(fingerprint_marker);
    const auto provider_begin = line.rfind(provider_marker);
    if (fingerprint_begin == std::string::npos ||
        provider_begin == std::string::npos ||
        provider_begin <= fingerprint_begin + fingerprint_marker.size()) {
        return;
    }
    const std::string model_fingerprint = line.substr(
        fingerprint_begin + fingerprint_marker.size(),
        provider_begin - (fingerprint_begin + fingerprint_marker.size()));
    const auto provider_value_begin = provider_begin + provider_marker.size();
    const auto provider_fingerprint_begin = line.find(
        provider_fingerprint_marker, provider_value_begin);
    const std::string provider = line.substr(
        provider_value_begin,
        provider_fingerprint_begin == std::string::npos
            ? std::string::npos
            : provider_fingerprint_begin - provider_value_begin);
    std::string provider_fingerprint;
    if (provider_fingerprint_begin != std::string::npos) {
        const auto value_begin =
            provider_fingerprint_begin + provider_fingerprint_marker.size();
        const auto value_end = line.find(':', value_begin);
        provider_fingerprint = line.substr(
            value_begin,
            value_end == std::string::npos ? std::string::npos : value_end - value_begin);
    }
    if (!decimal_fingerprint(model_fingerprint) || !safe_token(provider, 64) ||
        (!provider_fingerprint.empty() &&
         !decimal_fingerprint(provider_fingerprint))) {
        return;
    }
    result.runtime_model_fingerprint = model_fingerprint;
    result.runtime_provider = provider;
    result.runtime_provider_fingerprint = provider_fingerprint;
}

bool metadata_line(const std::string_view line) {
    return line == "id" || line.starts_with("id ") ||
           line == "option" || line.starts_with("option ");
}

void capture_metadata_line(
    std::vector<std::string>& metadata_lines,
    const std::string& line) {
    if (line.size() > uci_metadata_maximum_line_bytes) {
        throw std::runtime_error("UCI metadata line exceeds its byte bound");
    }
    if (metadata_lines.size() >= uci_metadata_maximum_lines) {
        throw std::runtime_error("UCI metadata exceeds its line-count bound");
    }
    metadata_lines.push_back(line);
}

std::string await_token(
    ports::NativeProcess& process,
    const std::string_view token,
    const Clock::time_point deadline,
    UciProbeResult& result,
    std::vector<std::string>* metadata_lines = nullptr) {
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
        if (metadata_lines != nullptr && metadata_line(*line)) {
            capture_metadata_line(*metadata_lines, *line);
        }
        capture_runtime_model_identity(*line, result);
        if (*line == token || line->starts_with(std::string(token) + " ")) {
            return *line;
        }
    }
    throw std::runtime_error("engine timed out waiting for " + std::string(token));
}

UciMetadata parse_metadata(const std::vector<std::string>& lines) {
    std::vector<std::string_view> views;
    views.reserve(lines.size());
    for (const std::string& line : lines) {
        views.push_back(line);
    }
    return UciOptionContract::parse(
        std::span<const std::string_view>(views.data(), views.size()));
}

CanonicalUciOptions canonicalize_options(
    const std::vector<domain::UciOption>& advertised_options,
    const std::map<std::string, std::string>& option_overrides) {
    try {
        return UciOptionContract::canonicalize_overrides(
            std::span<const domain::UciOption>(
                advertised_options.data(), advertised_options.size()),
            option_overrides);
    } catch (const std::runtime_error& error) {
        if (std::string_view(error.what()) ==
            "UCI override names an unadvertised option") {
            std::set<std::string, std::less<>> advertised_names;
            for (const domain::UciOption& option : advertised_options) {
                advertised_names.insert(ascii_fold(option.name));
            }
            for (const auto& [name, value] : option_overrides) {
                static_cast<void>(value);
                if (!advertised_names.contains(ascii_fold(name))) {
                    throw std::runtime_error(
                        "requested UCI option is not advertised: " + name);
                }
            }
        }
        throw;
    }
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

void UciProcess::validate_option_overrides(
    const std::vector<domain::UciOption>& advertised_options,
    const std::map<std::string, std::string>& option_overrides) {
    static_cast<void>(canonicalize_options(advertised_options, option_overrides));
}

UciProbeResult UciProcess::probe(
    const std::filesystem::path& executable,
    const std::filesystem::path& working_directory,
    const std::vector<std::string>& arguments,
    std::chrono::milliseconds timeout,
    const std::map<std::string, std::string>& option_overrides) {
    UciProbeResult result;
    const auto started = Clock::now();
    ports::NativeProcess process;
    try {
        result.fingerprint = fingerprint(executable);
        process.start(executable, working_directory, arguments);
        const auto deadline = Clock::now() + timeout;

        std::vector<std::string> metadata_lines;
        process.write_line("uci");
        await_token(process, "uciok", deadline, result, &metadata_lines);
        UciMetadata metadata = parse_metadata(metadata_lines);
        result.engine_name = std::move(metadata.engine_name);
        result.engine_author = std::move(metadata.engine_author);
        result.options = std::move(metadata.options);

        const CanonicalUciOptions canonical_options =
            canonicalize_options(result.options, option_overrides);
        for (const auto& [name, value] : canonical_options) {
            process.write_line("setoption name " + name + " value " + value);
        }
        process.write_line("isready");
        await_token(process, "readyok", deadline, result);
        process.write_line("ucinewgame");
        process.write_line("position startpos");
        process.write_line("go depth 1");
        const std::string bestmove_line =
            await_token(process, "bestmove", deadline, result);
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
