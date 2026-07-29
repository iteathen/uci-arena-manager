#include "arena/application/engine_onboarding.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <system_error>

namespace arena::application {
namespace {

namespace fs = std::filesystem;
using nlohmann::json;

constexpr std::uintmax_t max_metadata_bytes = 1024 * 1024;
constexpr std::size_t max_receipts = 1024;
constexpr std::size_t max_discovered_executables = 512;

json read_json_bounded(const fs::path& path) {
    std::error_code error;
    const auto size = fs::file_size(path, error);
    if (error || size > max_metadata_bytes) {
        throw std::runtime_error("metadata file is unavailable or exceeds one MiB");
    }
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        throw std::runtime_error("metadata file cannot be opened");
    }
    json value;
    input >> value;
    return value;
}

fs::path normalized_absolute(const fs::path& value) {
    std::error_code error;
    fs::path result = fs::weakly_canonical(value, error);
    if (error) {
        result = fs::absolute(value, error);
    }
    return (error ? value : result).lexically_normal();
}

bool safe_relative_path(const fs::path& value) {
    if (value.empty() || value.is_absolute()) {
        return false;
    }
    return std::none_of(
        value.begin(), value.end(),
        [](const fs::path& part) { return part == ".."; });
}

bool usable_executable(const fs::path& value) {
    std::error_code error;
    if (!fs::is_regular_file(value, error) || error ||
        fs::is_symlink(value, error) || error) {
        return false;
    }
#ifdef _WIN32
    std::string extension = value.extension().string();
    std::ranges::transform(
        extension, extension.begin(),
        [](const unsigned char character) {
            return static_cast<char>(std::tolower(character));
        });
    return extension == ".exe";
#else
    const auto permissions = fs::status(value, error).permissions();
    if (error) {
        return false;
    }
    using fs::perms;
    return (permissions &
            (perms::owner_exec | perms::group_exec | perms::others_exec)) !=
           perms::none;
#endif
}

std::string path_key(const fs::path& value) {
    std::string key = normalized_absolute(value).generic_string();
#ifdef _WIN32
    std::ranges::transform(
        key, key.begin(),
        [](const unsigned char character) {
            return static_cast<char>(std::tolower(character));
        });
#endif
    return key;
}

std::string candidate_id(const fs::path& executable) {
    std::uint64_t hash = 1469598103934665603ULL;
    for (const unsigned char character : path_key(executable)) {
        hash ^= character;
        hash *= 1099511628211ULL;
    }
    std::ostringstream output;
    output << "engcand-" << std::hex << std::setfill('0')
           << std::setw(16) << hash;
    return output.str();
}

std::string display_name_for(const fs::path& executable) {
    std::string value = executable.stem().string();
    std::replace(value.begin(), value.end(), '_', ' ');
    std::replace(value.begin(), value.end(), '-', ' ');
    return value.empty() ? "UCI engine" : value;
}

void add_candidate(
    json& candidates,
    std::set<std::string>& seen,
    const std::string& source,
    const fs::path& executable,
    const fs::path& working_directory,
    const std::string& display_name,
    const json& metadata = json::object()) {
    const fs::path normalized_executable = normalized_absolute(executable);
    if (!seen.insert(path_key(normalized_executable)).second) {
        return;
    }
    json issues = json::array();
    const bool usable = usable_executable(normalized_executable);
    if (!usable) {
        issues.push_back({
            {"code", "engine_executable_unavailable"},
            {"detail",
             "The Manager service cannot access a regular executable at this path."},
        });
    }
    json candidate{
        {"schema", "arena_engine_candidate_v1"},
        {"schema_version", 1},
        {"candidate_id", candidate_id(normalized_executable)},
        {"source", source},
        {"state", usable ? "ready_to_probe" : "unavailable"},
        {"display_name",
         display_name.empty() ? display_name_for(normalized_executable) :
                                display_name},
        {"executable", normalized_executable.string()},
        {"working_directory",
         normalized_absolute(working_directory).string()},
        {"arguments", json::array()},
        {"uci_options", json::object()},
        {"issues", std::move(issues)},
    };
    for (const auto& [key, value] : metadata.items()) {
        candidate[key] = value;
    }
    candidates.push_back(std::move(candidate));
}

void append_receipt_candidates(
    const fs::path& data_root,
    json& candidates,
    std::set<std::string>& seen) {
    const fs::path receipts_root = data_root / "receipts";
    std::error_code error;
    if (!fs::is_directory(receipts_root, error) ||
        fs::is_symlink(receipts_root, error)) {
        return;
    }
    std::vector<fs::path> receipts;
    for (const auto& entry : fs::directory_iterator(
             receipts_root, fs::directory_options::skip_permission_denied,
             error)) {
        if (receipts.size() >= max_receipts) {
            break;
        }
        if (entry.is_regular_file(error) && !entry.is_symlink(error) &&
            entry.path().extension() == ".json") {
            receipts.push_back(entry.path());
        }
    }
    std::ranges::sort(receipts);
    for (const auto& receipt_path : receipts) {
        try {
            const json receipt = read_json_bounded(receipt_path);
            if (receipt.value("schema", "") != "arena_install_receipt_v1" ||
                receipt.value("schema_version", 0) != 1 ||
                receipt.value("state", "") != "installed_managed" ||
                !receipt.contains("active_root")) {
                continue;
            }
            const fs::path active_root =
                normalized_absolute(receipt.at("active_root").get<std::string>());
            const json manifest =
                read_json_bounded(active_root / "arena-component.json");
            if (manifest.value("schema", "") !=
                    "arena_install_component_v1" ||
                manifest.value("component_id", "") !=
                    receipt.value("component_id", "") ||
                manifest.value("component_version", "") !=
                    receipt.value("component_version", "") ||
                !manifest.contains("entrypoints") ||
                !manifest.at("entrypoints").is_object() ||
                !manifest.at("entrypoints").contains("uci")) {
                continue;
            }
            const fs::path relative =
                manifest.at("entrypoints").at("uci").get<std::string>();
            if (!safe_relative_path(relative)) {
                continue;
            }
            const fs::path executable = active_root / relative;
            add_candidate(
                candidates, seen, "install_receipt", executable, active_root,
                manifest.value("display_name", display_name_for(executable)),
                {
                    {"component_id", receipt.value("component_id", "")},
                    {"component_version",
                     receipt.value("component_version", "")},
                });
        } catch (const std::exception&) {
        }
    }
}

void append_saved_locator(
    const fs::path& data_root,
    json& candidates,
    std::set<std::string>& seen) {
    const fs::path locator_path =
        data_root / "locators" / "uci_engine.json";
    std::error_code error;
    if (!fs::is_regular_file(locator_path, error) ||
        fs::is_symlink(locator_path, error)) {
        return;
    }
    try {
        const json locator = read_json_bounded(locator_path);
        if (locator.value("schema", "") != "arena_saved_locator_v1" ||
            locator.value("schema_version", 0) != 1 ||
            locator.value("kind", "") != "uci_engine" ||
            !locator.contains("path")) {
            return;
        }
        const fs::path executable =
            normalized_absolute(locator.at("path").get<std::string>());
        const json validation =
            locator.value("validation", json::object());
        json metadata = json::object();
        if (validation.contains("fingerprint") &&
            validation.at("fingerprint").is_string()) {
            metadata["fingerprint"] = validation.at("fingerprint");
        }
        add_candidate(
            candidates, seen, "saved_locator", executable,
            executable.parent_path(),
            validation.value("engine_name", display_name_for(executable)),
            metadata);
    } catch (const std::exception&) {
    }
}

void append_explicit_root(
    const fs::path& supplied_root,
    json& candidates,
    std::set<std::string>& seen) {
    const fs::path root = normalized_absolute(supplied_root);
    std::error_code error;
    if (fs::is_symlink(root, error)) {
        return;
    }
    if (fs::is_regular_file(root, error)) {
        add_candidate(
            candidates, seen, "discovered", root, root.parent_path(),
            display_name_for(root));
        return;
    }
    if (!fs::is_directory(root, error)) {
        return;
    }
    std::size_t inspected = 0;
    fs::recursive_directory_iterator iterator(
        root, fs::directory_options::skip_permission_denied, error);
    const fs::recursive_directory_iterator end;
    for (; iterator != end && inspected < max_discovered_executables;
         iterator.increment(error)) {
        if (error) {
            error.clear();
            continue;
        }
        const bool is_dir = iterator->is_directory(error);
        if (error) {
            error.clear();
            continue;
        }
        if (iterator.depth() >= 1 && is_dir) {
            iterator.disable_recursion_pending();
        }
        const bool is_sym = iterator->is_symlink(error);
        if (error) {
            error.clear();
            continue;
        }
        const bool is_reg = iterator->is_regular_file(error);
        if (error) {
            error.clear();
            continue;
        }
        if (iterator.depth() > 1 || is_sym || !is_reg) {
            continue;
        }
        ++inspected;
        if (usable_executable(iterator->path())) {
            add_candidate(
                candidates, seen, "discovered", iterator->path(),
                iterator->path().parent_path(),
                display_name_for(iterator->path()));
        }
    }
}

void append_path_candidates(
    json& candidates,
    std::set<std::string>& seen) {
    const char* path_environment = std::getenv("PATH");
    if (path_environment == nullptr) {
        return;
    }
#ifdef _WIN32
    constexpr char separator = ';';
    constexpr std::array names{
        "uci_arena_engine.exe", "stockfish.exe", "lc0.exe"};
#else
    constexpr char separator = ':';
    constexpr std::array names{"uci_arena_engine", "stockfish", "lc0"};
#endif
    std::stringstream paths(path_environment);
    for (std::string item; std::getline(paths, item, separator);) {
        if (item.empty()) {
            continue;
        }
        for (const auto* name : names) {
            const fs::path executable = fs::path(item) / name;
            if (usable_executable(executable)) {
                add_candidate(
                    candidates, seen, "discovered", executable,
                    executable.parent_path(), display_name_for(executable));
            }
        }
    }
}

}  // namespace

json EngineOnboarding::discover(
    const fs::path& manager_data_root,
    const std::vector<fs::path>& explicit_roots,
    const bool include_path_candidates) {
    json candidates = json::array();
    std::set<std::string> seen;
    append_receipt_candidates(manager_data_root, candidates, seen);
    append_saved_locator(manager_data_root, candidates, seen);
    for (const auto& root : explicit_roots) {
        append_explicit_root(root, candidates, seen);
    }
    if (include_path_candidates) {
        append_path_candidates(candidates, seen);
    }
    return {
        {"schema", "arena_engine_candidates_v1"},
        {"schema_version", 1},
        {"precedence",
         {"install_receipt", "saved_locator", "provider_manifest",
          "discovered"}},
        {"candidates", std::move(candidates)},
    };
}

}  // namespace arena::application
