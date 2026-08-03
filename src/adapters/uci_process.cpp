#include "arena/adapters/uci_process.hpp"

#include "arena/ports/native_process.hpp"

#include <sodium.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <chrono>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string_view>

namespace arena::adapters {
namespace {

using Clock = std::chrono::steady_clock;
namespace fs = std::filesystem;

constexpr std::size_t max_arguments = 64;
constexpr std::size_t max_argument_bytes = 4096;
constexpr std::size_t max_uci_line_bytes = 64 * 1024;
constexpr std::size_t max_identity_bytes = 256;
constexpr std::size_t max_options = 256;
constexpr std::size_t max_option_name_bytes = 128;
constexpr std::size_t max_option_value_bytes = 512;
constexpr std::size_t max_option_variants = 128;
constexpr std::size_t max_option_variant_bytes = 256;

bool bounded_text(
    const std::string_view value,
    const std::size_t maximum,
    const bool allow_empty = false) {
    return (allow_empty || !value.empty()) && value.size() <= maximum &&
           std::ranges::none_of(value, [](const unsigned char character) {
               return character == 0 || character == '\r' || character == '\n' ||
                      (character < 0x20 && character != '\t') || character == 0x7f;
           });
}

void validate_process_inputs(
    const fs::path& executable,
    const fs::path& working_directory,
    const std::vector<std::string>& arguments,
    const std::chrono::milliseconds timeout,
    const std::map<std::string, std::string, std::less<>>& option_overrides) {
    std::error_code error;
    const auto executable_status = fs::symlink_status(executable, error);
    if (error || !fs::is_regular_file(executable_status) ||
        fs::is_symlink(executable_status)) {
        throw std::runtime_error(
            "engine executable must be a regular non-symlink file");
    }
    const auto working_status = fs::symlink_status(working_directory, error);
    if (error || !fs::is_directory(working_status) ||
        fs::is_symlink(working_status)) {
        throw std::runtime_error(
            "engine working directory must be a non-symlink directory");
    }
    if (arguments.size() > max_arguments) {
        throw std::runtime_error("engine argument count exceeds 64");
    }
    for (const auto& argument : arguments) {
        if (!bounded_text(argument, max_argument_bytes, true)) {
            throw std::runtime_error("engine argument is invalid or too large");
        }
    }
    if (timeout <= std::chrono::milliseconds::zero() ||
        timeout > std::chrono::minutes(1)) {
        throw std::runtime_error("UCI probe timeout must be in 1 ms..60 s");
    }
    if (option_overrides.size() > 128) {
        throw std::runtime_error("UCI option override count exceeds 128");
    }
    for (const auto& [name, value] : option_overrides) {
        if (!bounded_text(name, max_option_name_bytes) ||
            !bounded_text(value, max_option_value_bytes, true)) {
            throw std::runtime_error("UCI option override is invalid or too large");
        }
    }
}

std::string fingerprint(const fs::path& executable) {
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
                &state,
                buffer.data(),
                static_cast<unsigned long long>(count));
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

std::int64_t parse_integer(
    const std::string& value,
    const std::string& field) {
    std::int64_t result = 0;
    const auto parsed =
        std::from_chars(value.data(), value.data() + value.size(), result);
    if (parsed.ec != std::errc{} ||
        parsed.ptr != value.data() + value.size()) {
        throw std::runtime_error(
            "UCI option has an invalid " + field + " value");
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
        } else {
            throw std::runtime_error("UCI option contains an unknown field");
        }
    }
    return option;
}

void validate_options(UciProbeResult& result) {
    static const std::set<std::string, std::less<>> allowed_types{
        "button", "check", "combo", "spin", "string"};
    if (result.options.size() > max_options) {
        throw std::runtime_error("engine advertises more than 256 UCI options");
    }
    std::set<std::string, std::less<>> names;
    for (const auto& option : result.options) {
        if (!bounded_text(option.name, max_option_name_bytes) ||
            !allowed_types.contains(option.type) ||
            !bounded_text(
                option.default_value,
                max_option_value_bytes,
                true) ||
            option.variants.size() > max_option_variants) {
            throw std::runtime_error("engine advertises an invalid UCI option");
        }
        if (!names.insert(option.name).second) {
            throw std::runtime_error(
                "engine advertises a duplicate UCI option: " + option.name);
        }
        for (const auto& variant : option.variants) {
            if (!bounded_text(variant, max_option_variant_bytes)) {
                throw std::runtime_error(
                    "engine advertises an invalid UCI option variant");
            }
        }
        if (option.minimum.has_value() && option.maximum.has_value() &&
            *option.minimum > *option.maximum) {
            throw std::runtime_error(
                "engine advertises a UCI option with min greater than max");
        }
    }
}

std::string value_after(
    const std::string& line,
    const std::string_view prefix) {
    return line.starts_with(prefix)
        ? line.substr(prefix.size())
        : std::string{};
}

void capture_identity(
    const std::string& value,
    const char* field,
    std::string& target) {
    if (!bounded_text(value, max_identity_bytes)) {
        throw std::runtime_error(
            std::string("engine ") + field + " is invalid or too large");
    }
    if (!target.empty() && target != value) {
        throw std::runtime_error(
            std::string("engine advertises conflicting ") + field);
    }
    target = value;
}

bool safe_token(
    const std::string_view value,
    const std::size_t maximum) {
    return !value.empty() && value.size() <= maximum &&
           std::ranges::all_of(value, [](const unsigned char character) {
               return std::isalnum(character) != 0 || character == '_' ||
                      character == '-' || character == '.';
           });
}

bool decimal_fingerprint(const std::string_view value) {
    return !value.empty() && value.size() <= 20 &&
           std::ranges::all_of(value, [](const unsigned char character) {
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
        provider_begin -
            (fingerprint_begin + fingerprint_marker.size()));
    const auto provider_value_begin =
        provider_begin + provider_marker.size();
    const auto provider_fingerprint_begin = line.find(
        provider_fingerprint_marker,
        provider_value_begin);
    const std::string provider = line.substr(
        provider_value_begin,
        provider_fingerprint_begin == std::string::npos
            ? std::string::npos
            : provider_fingerprint_begin - provider_value_begin);
    std::string provider_fingerprint;
    if (provider_fingerprint_begin != std::string::npos) {
        const auto value_begin =
            provider_fingerprint_begin +
            provider_fingerprint_marker.size();
        const auto value_end = line.find(':', value_begin);
        provider_fingerprint = line.substr(
            value_begin,
            value_end == std::string::npos
                ? std::string::npos
                : value_end - value_begin);
    }
    if (!decimal_fingerprint(model_fingerprint) ||
        !safe_token(provider, 64) ||
        (!provider_fingerprint.empty() &&
         !decimal_fingerprint(provider_fingerprint))) {
        return;
    }
    result.runtime_model_fingerprint = model_fingerprint;
    result.runtime_provider = provider;
    result.runtime_provider_fingerprint = provider_fingerprint;
}

std::string await_token(
    ports::NativeProcess& process,
    const std::string_view token,
    const Clock::time_point deadline,
    UciProbeResult& result,
    const bool capture_metadata) {
    while (Clock::now() < deadline) {
        const auto remaining =
            std::chrono::duration_cast<std::chrono::milliseconds>(
                deadline - Clock::now());
        const auto line = process.read_line(
            std::min(remaining, std::chrono::milliseconds(250)));
        if (!line) {
            if (!process.running()) {
                throw std::runtime_error(
                    "engine exited before " + std::string(token));
            }
            continue;
        }
        if (line->size() > max_uci_line_bytes) {
            throw std::runtime_error("engine emitted an oversized UCI line");
        }
        if (capture_metadata) {
            if (line->starts_with("id name ")) {
                capture_identity(
                    value_after(*line, "id name "),
                    "name",
                    result.engine_name);
            } else if (line->starts_with("id author ")) {
                capture_identity(
                    value_after(*line, "id author "),
                    "author",
                    result.engine_author);
            } else if (line->starts_with("option ")) {
                if (result.options.size() >= max_options) {
                    throw std::runtime_error(
                        "engine advertises more than 256 UCI options");
                }
                auto option = parse_option(*line);
                if (option.name.empty() || option.type.empty()) {
                    throw std::runtime_error(
                        "engine advertises a malformed UCI option");
                }
                result.options.push_back(std::move(option));
            }
        }
        capture_runtime_model_identity(*line, result);
        if (*line == token ||
            line->starts_with(std::string(token) + " ")) {
            return *line;
        }
    }
    throw std::runtime_error(
        "engine timed out waiting for " + std::string(token));
}

bool legal_starting_bestmove(const std::string& move) {
    static const std::set<std::string, std::less<>> legal = {
        "a2a3", "a2a4", "b2b3", "b2b4", "c2c3", "c2c4", "d2d3",
        "d2d4", "e2e3", "e2e4", "f2f3", "f2f4", "g2g3", "g2g4",
        "h2h3", "h2h4", "b1a3", "b1c3", "g1f3", "g1h3",
    };
    return legal.contains(move);
}

template<typename MapType>
void validate_option_overrides_impl(
    const std::vector<domain::UciOption>& advertised_options,
    const MapType& option_overrides) {
    if (option_overrides.size() > 128) {
        throw std::runtime_error("UCI option override count exceeds 128");
    }
    std::map<std::string, const domain::UciOption*, std::less<>> by_name;
    for (const auto& option : advertised_options) {
        if (option.name.empty() || !by_name.emplace(option.name, &option).second) {
            throw std::runtime_error(
                "engine advertised an empty or duplicate UCI option name");
        }
    }
    for (const auto& [name, value] : option_overrides) {
        if (!bounded_text(name, max_option_name_bytes) ||
            !bounded_text(value, max_option_value_bytes, true)) {
            throw std::runtime_error("UCI option override is invalid or too large");
        }
        const auto found = by_name.find(name);
        if (found == by_name.end()) {
            throw std::runtime_error(
                "requested UCI option is not advertised: " + name);
        }
        const domain::UciOption& option = *found->second;
        if (option.type == "button") {
            throw std::runtime_error(
                "UCI button options cannot be persisted as values: " + name);
        }
        if (option.type == "check" && value != "true" && value != "false") {
            throw std::runtime_error(
                "UCI check option requires true or false: " + name);
        }
        if (option.type == "spin") {
            const std::int64_t number = parse_integer(value, "spin");
            if ((option.minimum && number < *option.minimum) ||
                (option.maximum && number > *option.maximum)) {
                throw std::runtime_error(
                    "UCI spin option is outside its advertised bounds: " + name);
            }
        }
        if (option.type == "combo" && !option.variants.empty() &&
            std::ranges::find(option.variants, value) == option.variants.end()) {
            throw std::runtime_error(
                "UCI combo option is not an advertised variant: " + name);
        }
    }
}

}  // namespace

void UciProcess::validate_option_overrides(
    const std::vector<domain::UciOption>& advertised_options,
    const std::map<std::string, std::string>& option_overrides) {
    validate_option_overrides_impl(advertised_options, option_overrides);
}

void UciProcess::validate_option_overrides(
    const std::vector<domain::UciOption>& advertised_options,
    const std::map<std::string, std::string, std::less<>>& option_overrides) {
    validate_option_overrides_impl(advertised_options, option_overrides);
}

UciProbeResult UciProcess::probe(
    const std::filesystem::path& executable,
    const std::filesystem::path& working_directory,
    const std::vector<std::string>& arguments,
    const std::chrono::milliseconds timeout,
    const std::map<std::string, std::string, std::less<>>& option_overrides) {
    UciProbeResult result;
    const auto started = Clock::now();
    ports::NativeProcess process;
    try {
        validate_process_inputs(
            executable,
            working_directory,
            arguments,
            timeout,
            option_overrides);
        result.fingerprint = fingerprint(executable);
        process.start(executable, working_directory, arguments);
        const auto deadline = Clock::now() + timeout;

        process.write_line("uci");
        await_token(process, "uciok", deadline, result, true);
        validate_options(result);
        validate_option_overrides(result.options, option_overrides);
        for (const auto& [name, value] : option_overrides) {
            process.write_line("setoption name " + name + " value " + value);
        }
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
