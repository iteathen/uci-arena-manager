#include "arena/application/service_catalog.hpp"

#include <sodium.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <set>
#include <stdexcept>
#include <tuple>
#include <vector>

namespace arena::application {
namespace {

using nlohmann::json;
namespace fs = std::filesystem;

json read_json_file(const fs::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        throw std::runtime_error("cannot read installed component metadata");
    }
    return json::parse(input);
}

std::optional<fs::path> saved_locator_reference(
    const fs::path& manager_data_root,
    const std::string& kind,
    const bool allow_directory = false) {
    const fs::path metadata =
        manager_data_root / "locators" / (kind + ".json");
    if (!fs::is_regular_file(metadata) || fs::is_symlink(metadata) ||
        fs::file_size(metadata) > 1024ULL * 1024ULL) {
        return std::nullopt;
    }
    const json locator = read_json_file(metadata);
    if (locator.value("schema", "") != "arena_saved_locator_v1" ||
        locator.value("schema_version", 0) != 1 ||
        locator.value("kind", "") != kind ||
        !locator.contains("path") || !locator.at("path").is_string()) {
        return std::nullopt;
    }
    const fs::path target =
        fs::absolute(locator.at("path").get<std::string>())
            .lexically_normal();
    if ((!fs::is_regular_file(target) &&
         !(allow_directory && fs::is_directory(target))) ||
        fs::is_symlink(target)) {
        return std::nullopt;
    }
    return target;
}

void project_bot_prerequisite(
    json& readiness,
    json& issues,
    const std::string& dimension,
    const bool ready,
    const std::string& issue_code,
    const std::string& detail) {
    readiness[dimension] = ready ? "ready" : "missing";
    if (!ready) {
        issues.push_back({
            {"code", issue_code},
            {"detail", detail},
        });
    }
}

std::set<std::string> retained_catalog_components(
    const fs::path& manager_data_root) {
    std::set<std::string> result;
    const fs::path catalog_path =
        manager_data_root / "release" / "catalog.json";
    if (!fs::is_regular_file(catalog_path) ||
        fs::is_symlink(catalog_path) ||
        fs::file_size(catalog_path) > 16ULL * 1024ULL * 1024ULL) {
        return result;
    }
    try {
        const json catalog = read_json_file(catalog_path);
        if (catalog.value("schema", "") != "arena_install_catalog_v1" ||
            !catalog.contains("components") ||
            !catalog.at("components").is_array()) {
            return result;
        }
        for (const auto& component : catalog.at("components")) {
            const std::string id = component.value("component_id", "");
            if (!id.empty()) {
                result.insert(id);
            }
        }
    } catch (const std::exception&) {
    }
    return result;
}

bool safe_relative_path(const fs::path& path) {
    if (path.empty() || path.is_absolute() || path.has_root_name()) {
        return false;
    }
    for (const auto& part : path) {
        if (part == "." || part == "..") {
            return false;
        }
    }
    return true;
}

bool within(const fs::path& root, const fs::path& candidate) {
    const fs::path normalized_root =
        fs::absolute(root).lexically_normal();
    const fs::path normalized_candidate =
        fs::absolute(candidate).lexically_normal();
    auto root_part = normalized_root.begin();
    auto candidate_part = normalized_candidate.begin();
    for (; root_part != normalized_root.end();
         ++root_part, ++candidate_part) {
        if (candidate_part == normalized_candidate.end() ||
            *candidate_part != *root_part) {
            return false;
        }
    }
    return normalized_candidate != normalized_root;
}

bool same_or_within(const fs::path& root, const fs::path& candidate) {
    return fs::absolute(root).lexically_normal() ==
               fs::absolute(candidate).lexically_normal() ||
           within(root, candidate);
}

bool valid_lichess_account_name(const std::string& value) {
    return !value.empty() && value.size() <= 64 &&
           std::ranges::all_of(
               value,
               [](const unsigned char character) {
                   return std::isalnum(character) != 0 ||
                          character == '_' || character == '-';
               });
}

std::string sha256_file(const fs::path& path) {
    if (sodium_init() < 0) {
        throw std::runtime_error("libsodium initialization failed");
    }
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        throw std::runtime_error("cannot verify installed entrypoint");
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
    sodium_bin2hex(
        encoded.data(), encoded.size(), digest.data(), digest.size());
    return encoded.data();
}

struct InstalledComponent {
    json receipt;
    json manifest;
    fs::path active_root;
};

void verify_component_files(
    const fs::path& active_root,
    const json& manifest,
    const std::string& component_id) {
    if (!manifest.contains("files") || !manifest.at("files").is_array()) {
        throw std::runtime_error(
            component_id + " component manifest has no file inventory");
    }
    std::set<fs::path> listed;
    for (const auto& file : manifest.at("files")) {
        if (!file.is_object() || !file.contains("path") ||
            !file.at("path").is_string()) {
            throw std::runtime_error(
                component_id + " component file inventory is malformed");
        }
        const fs::path relative = file.at("path").get<std::string>();
        if (!safe_relative_path(relative) ||
            !listed.insert(relative.lexically_normal()).second) {
            throw std::runtime_error(
                component_id + " component file inventory is unsafe");
        }
        const fs::path path = active_root / relative;
        if (!fs::is_regular_file(path) || fs::is_symlink(path) ||
            fs::file_size(path) != file.value("size_bytes", 0ULL) ||
            sha256_file(path) != file.value("sha256", "")) {
            throw std::runtime_error(
                component_id + " component file digest mismatch: " +
                relative.generic_string());
        }
    }
    for (const auto& entry : fs::recursive_directory_iterator(active_root)) {
        if (entry.is_symlink()) {
            throw std::runtime_error(
                component_id + " component contains a symbolic link");
        }
        if (!entry.is_regular_file()) {
            continue;
        }
        const fs::path relative =
            fs::relative(entry.path(), active_root).lexically_normal();
        if (relative != fs::path("arena-component.json") &&
            !listed.contains(relative)) {
            throw std::runtime_error(
                component_id + " component contains an unlisted file: " +
                relative.generic_string());
        }
    }
}

InstalledComponent installed_component(
    const fs::path& receipts_root,
    const std::string& component_id,
    const bool allow_disabled = false) {
    const json receipt =
        read_json_file(receipts_root / (component_id + ".json"));
    const std::string state = receipt.value("state", "");
    if (receipt.value("schema", "") != "arena_install_receipt_v1" ||
        receipt.value("component_id", "") != component_id ||
        (state != "installed_managed" &&
         !(allow_disabled && state == "installed_disabled"))) {
        throw std::runtime_error(
            component_id +
            (allow_disabled
                 ? " does not have a managed installation receipt"
                 : " does not have an enabled managed receipt"));
    }
    const fs::path active_root =
        receipt.at("active_root").get<std::string>();
    const json manifest = read_json_file(active_root / "arena-component.json");
    if (manifest.value("schema", "") != "arena_install_component_v1" ||
        manifest.value("component_id", "") != component_id ||
        manifest.value("component_version", "") !=
            receipt.value("component_version", "")) {
        throw std::runtime_error(
            component_id + " receipt and component manifest disagree");
    }
    verify_component_files(active_root, manifest, component_id);
    return {receipt, manifest, active_root};
}

fs::path verified_entrypoint(
    const InstalledComponent& component,
    const std::string& name) {
    if (!component.manifest.contains("entrypoints") ||
        !component.manifest.at("entrypoints").contains(name) ||
        !component.manifest.at("entrypoints").at(name).is_string()) {
        throw std::runtime_error(
            component.receipt.at("component_id").get<std::string>() +
            " does not declare the " + name + " entrypoint");
    }
    const fs::path relative =
        component.manifest.at("entrypoints").at(name).get<std::string>();
    if (!safe_relative_path(relative)) {
        throw std::runtime_error("installed component entrypoint is unsafe");
    }
    const fs::path path = component.active_root / relative;
    for (const auto& file : component.manifest.at("files")) {
        if (file.value("path", "") == relative.generic_string()) {
            if (!fs::is_regular_file(path) ||
                fs::file_size(path) != file.value("size_bytes", 0ULL) ||
                sha256_file(path) != file.value("sha256", "")) {
                throw std::runtime_error(
                    "installed component entrypoint digest mismatch");
            }
            return path;
        }
    }
    throw std::runtime_error(
        "installed component entrypoint is not in its signed file manifest");
}

json book_forge_profile() {
    return {
        {"profile_id", "book_forge"},
        {"display_name", "Book Forge"},
        {"implementation_state", "implemented_external"},
        {"optional", true},
        {"description", "Continuous opening-book acquisition, analysis, and publication"},
        {"capabilities",
         {
             "status",
             "start",
             "stop",
             "restart",
             "pause",
             "resume",
             "acquisition_status",
             "acquisition_run",
             "acquisition_retry_auth",
             "snapshot_build",
             "validate",
             "publish",
             "maintain",
             "startup_install",
             "startup_status",
             "startup_uninstall",
         }},
        {"required_configuration", {"node_executable", "cli_entrypoint", "workspace"}},
        {"discovery",
         {
             {"supported", true},
             {"manual_fallback", true},
             {"sources", {"process_environment_hints", "executable_path"}},
         }},
        {"health_contract",
         {
             {"schema", "uci_arena_book_forge_status_v2"},
             {"heartbeat_seconds", 15},
             {"stale_seconds", 300},
         }},
    };
}

json evidence_service_profile() {
    return {
        {"profile_id", "uci_arena_evidence"},
        {"display_name", "UCI Arena Evidence Service"},
        {"implementation_state", "implemented_external"},
        {"optional", true},
        {"description",
         "Durable identity-bound timing, strength evidence, datasets, and qualified profiles"},
        {"capabilities",
         {
             "status",
             "start",
             "stop",
             "restart",
             "pause",
             "resume",
             "init",
             "target_add",
             "target_list",
             "target_remove",
             "job_enqueue",
             "job_list",
             "job_cancel",
             "job_retry",
             "dataset_list",
             "dataset_show",
             "dataset_export",
             "profile_list",
             "prune",
             "startup_install",
             "startup_status",
             "startup_uninstall",
         }},
        {"required_configuration", {"node_executable", "cli_entrypoint", "workspace"}},
        {"discovery",
         {
             {"supported", true},
             {"manual_fallback", true},
             {"sources", {"process_environment_hints", "executable_path"}},
         }},
        {"health_contract",
         {
             {"schema", "uci_arena_evidence_status_v1"},
             {"heartbeat_seconds", 15},
             {"stale_seconds", 300},
         }},
    };
}

std::filesystem::path required_path(const json& object, const char* key) {
    if (!object.contains(key) || !object.at(key).is_string() ||
        object.at(key).get<std::string>().empty()) {
        throw std::runtime_error(std::string("service configuration requires ") + key);
    }
    return object.at(key).get<std::string>();
}

std::optional<std::string> environment_value(const char* key) {
#ifdef _WIN32
    char* buffer = nullptr;
    std::size_t length = 0;
    if (_dupenv_s(&buffer, &length, key) != 0 || buffer == nullptr) {
        return std::nullopt;
    }
    std::string value(buffer);
    std::free(buffer);
    if (value.empty()) {
        return std::nullopt;
    }
    return value;
#else
    const char* value = std::getenv(key);
    if (value == nullptr || *value == '\0') {
        return std::nullopt;
    }
    return std::string(value);
#endif
}

std::optional<std::filesystem::path> executable_on_path(const std::string& name) {
    const auto path_value = environment_value("PATH");
    if (!path_value) {
        return std::nullopt;
    }
#ifdef _WIN32
    constexpr char separator = ';';
    const std::vector<std::string> names{name + ".exe", name};
#else
    constexpr char separator = ':';
    const std::vector<std::string> names{name};
#endif
    std::size_t begin = 0;
    while (begin <= path_value->size()) {
        const auto end = path_value->find(separator, begin);
        const std::filesystem::path directory =
            path_value->substr(begin, end == std::string::npos ? end : end - begin);
        if (!directory.empty()) {
            for (const auto& candidate_name : names) {
                const auto candidate = directory / candidate_name;
                if (std::filesystem::is_regular_file(candidate)) {
                    return std::filesystem::absolute(candidate);
                }
            }
        }
        if (end == std::string::npos) {
            break;
        }
        begin = end + 1;
    }
    return std::nullopt;
}

struct SavedServiceLocator {
    std::optional<fs::path> cli_entrypoint;
    std::optional<std::string> warning;
};

SavedServiceLocator saved_service_locator(
    const fs::path& manager_data_root,
    const std::string& kind,
    const std::string& component_id) {
    if (manager_data_root.empty()) {
        return {};
    }
    const fs::path locator_path =
        manager_data_root / "locators" / (kind + ".json");
    if (!fs::exists(locator_path)) {
        return {};
    }
    try {
        if (!fs::is_regular_file(locator_path) ||
            fs::is_symlink(locator_path) ||
            fs::file_size(locator_path) > 1024ULL * 1024ULL) {
            throw std::runtime_error("saved locator metadata is unsafe");
        }
        const json locator = read_json_file(locator_path);
        if (locator.value("schema", "") != "arena_saved_locator_v1" ||
            locator.value("schema_version", 0) != 1 ||
            locator.value("kind", "") != kind ||
            locator.value("mode", "") != "in_place_reference" ||
            !locator.contains("path") || !locator.at("path").is_string()) {
            throw std::runtime_error("saved locator metadata is incompatible");
        }
        const fs::path target =
            fs::absolute(locator.at("path").get<std::string>()).lexically_normal();
        if (fs::is_symlink(target)) {
            throw std::runtime_error("saved locator target is a symbolic link");
        }
        if (fs::is_regular_file(target)) {
            return {target, std::nullopt};
        }
        if (!fs::is_directory(target)) {
            throw std::runtime_error("saved locator target is unavailable");
        }
        const fs::path manifest_path = target / "arena-component.json";
        if (!fs::is_regular_file(manifest_path) ||
            fs::is_symlink(manifest_path) ||
            fs::file_size(manifest_path) > 1024ULL * 1024ULL) {
            throw std::runtime_error(
                "saved provider root has no safe component manifest");
        }
        const json manifest = read_json_file(manifest_path);
        if (manifest.value("schema", "") != "arena_install_component_v1" ||
            manifest.value("component_id", "") != component_id ||
            !manifest.contains("entrypoints") ||
            !manifest.at("entrypoints").is_object() ||
            !manifest.at("entrypoints").contains("cli") ||
            !manifest.at("entrypoints").at("cli").is_string()) {
            throw std::runtime_error(
                "saved provider manifest has an incompatible CLI contract");
        }
        const fs::path relative =
            manifest.at("entrypoints").at("cli").get<std::string>();
        if (!safe_relative_path(relative)) {
            throw std::runtime_error(
                "saved provider manifest has an unsafe CLI entrypoint");
        }
        const fs::path cli = target / relative;
        if (!within(target, cli) || !fs::is_regular_file(cli) ||
            fs::is_symlink(cli)) {
            throw std::runtime_error(
                "saved provider CLI entrypoint is unavailable");
        }
        return {fs::absolute(cli).lexically_normal(), std::nullopt};
    } catch (const std::exception& error) {
        return {std::nullopt, error.what()};
    }
}

}  // namespace

json ServiceCatalog::profiles() {
    return json::array({book_forge_profile(), evidence_service_profile()});
}

json ServiceCatalog::discover(
    const std::string& profile_id,
    const std::filesystem::path& manager_data_root) {
    json candidates = json::array();
    json manual_configuration = json::array();
    if (profile_id == "book_forge") {
        manual_configuration = {"node_executable", "cli_entrypoint", "workspace"};
        json configuration = json::object();
        const auto saved = saved_service_locator(
            manager_data_root, "book_forge", "book_forge.service");
        if (saved.cli_entrypoint) {
            configuration["cli_entrypoint"] =
                saved.cli_entrypoint->string();
        }
        if (const auto configured_node = environment_value("BOOK_FORGE_NODE")) {
            configuration["node_executable"] = *configured_node;
        } else if (const auto path_node = executable_on_path("node")) {
            configuration["node_executable"] = path_node->string();
        }
        if (const auto cli = environment_value("BOOK_FORGE_CLI")) {
            configuration["cli_entrypoint"] = *cli;
        }
        if (const auto workspace = environment_value("BOOK_FORGE_WORKSPACE")) {
            configuration["workspace"] = *workspace;
        }
        if (!configuration.empty()) {
            json missing = json::array();
            for (const auto* key : {"node_executable", "cli_entrypoint", "workspace"}) {
                if (!configuration.contains(key)) {
                    missing.push_back(key);
                }
            }
            candidates.push_back(
                {
                    {"source",
                     saved.cli_entrypoint || saved.warning
                         ? "saved_locator"
                         : "environment_and_path"},
                    {"complete", missing.empty()},
                    {"configuration", configuration},
                    {"missing", missing},
                    {"warning", saved.warning ? json(*saved.warning) : json(nullptr)},
                });
        } else if (saved.warning) {
            candidates.push_back(
                {
                    {"source", "saved_locator"},
                    {"complete", false},
                    {"configuration", configuration},
                    {"missing", manual_configuration},
                    {"warning", *saved.warning},
                });
        }
    } else if (profile_id == "uci_arena_evidence") {
        manual_configuration = {"node_executable", "cli_entrypoint", "workspace"};
        json configuration = json::object();
        const auto saved = saved_service_locator(
            manager_data_root,
            "evidence_service",
            "uci_arena_evidence.service");
        if (saved.cli_entrypoint) {
            configuration["cli_entrypoint"] =
                saved.cli_entrypoint->string();
        }
        if (const auto configured_node = environment_value("UCI_ARENA_EVIDENCE_NODE")) {
            configuration["node_executable"] = *configured_node;
        } else if (const auto path_node = executable_on_path("node")) {
            configuration["node_executable"] = path_node->string();
        }
        if (const auto cli = environment_value("UCI_ARENA_EVIDENCE_CLI")) {
            configuration["cli_entrypoint"] = *cli;
        }
        if (const auto workspace = environment_value("UCI_ARENA_EVIDENCE_ROOT")) {
            configuration["workspace"] = *workspace;
        }
        if (!configuration.empty()) {
            json missing = json::array();
            for (const auto* key : {"node_executable", "cli_entrypoint", "workspace"}) {
                if (!configuration.contains(key)) {
                    missing.push_back(key);
                }
            }
            candidates.push_back(
                {
                    {"source",
                     saved.cli_entrypoint || saved.warning
                         ? "saved_locator"
                         : "environment_and_path"},
                    {"complete", missing.empty()},
                    {"configuration", configuration},
                    {"missing", missing},
                    {"warning", saved.warning ? json(*saved.warning) : json(nullptr)},
                });
        } else if (saved.warning) {
            candidates.push_back(
                {
                    {"source", "saved_locator"},
                    {"complete", false},
                    {"configuration", configuration},
                    {"missing", manual_configuration},
                    {"warning", *saved.warning},
                });
        }
    } else {
        throw std::runtime_error("unknown managed service profile: " + profile_id);
    }
    return {
        {"schema_version", 1},
        {"profile_id", profile_id},
        {"optional", true},
        {"candidates", candidates},
        {"manual_configuration", manual_configuration},
    };
}

json ServiceCatalog::normalize_registration(const json& parameters) {
    if (!parameters.is_object()) {
        throw std::runtime_error("service parameters must be an object");
    }
    const std::string profile_id = parameters.value("profile_id", "");
    json data = parameters;
    data.erase("labels");
    if (profile_id == "book_forge") {
        const auto node = required_path(parameters, "node_executable");
        const auto entrypoint = required_path(parameters, "cli_entrypoint");
        const auto workspace = required_path(parameters, "workspace");
        if (!std::filesystem::is_regular_file(node)) {
            throw std::runtime_error("Book Forge Node executable is unavailable");
        }
        if (!std::filesystem::is_regular_file(entrypoint)) {
            throw std::runtime_error("Book Forge CLI entrypoint is unavailable");
        }
        if (!std::filesystem::is_directory(workspace)) {
            throw std::runtime_error("Book Forge workspace is unavailable");
        }
        data["node_executable"] = std::filesystem::absolute(node).string();
        data["cli_entrypoint"] = std::filesystem::absolute(entrypoint).string();
        data["workspace"] = std::filesystem::absolute(workspace).string();
        data["display_name"] = parameters.value("display_name", "Book Forge");
        data["integration_state"] = "configured";
        data["desired_state"] =
            parameters.value("startup_installed", false) ? "running" : "stopped";
        data["observed_state"] = "stopped";
        data["transport"] = "process";
        data["capabilities"] = book_forge_profile().at("capabilities");
        if (parameters.value("startup_installed", false)) {
            data["startup_control"] = "provider_owned_per_user";
            for (const auto* capability : {"start", "stop", "restart"}) {
                const auto iterator =
                    std::find(data["capabilities"].begin(),
                              data["capabilities"].end(), capability);
                if (iterator != data["capabilities"].end()) {
                    data["capabilities"].erase(iterator);
                }
            }
        }
        data["health"] = {
            {"state", "unknown"},
            {"reason", "status has not been probed"},
            {"stale_after_seconds", 300},
        };
        return data;
    }
    if (profile_id == "uci_arena_evidence") {
        const auto node = required_path(parameters, "node_executable");
        const auto entrypoint = required_path(parameters, "cli_entrypoint");
        const auto workspace = required_path(parameters, "workspace");
        if (!std::filesystem::is_regular_file(node)) {
            throw std::runtime_error("UCI Arena Evidence Service Node executable is unavailable");
        }
        if (!std::filesystem::is_regular_file(entrypoint)) {
            throw std::runtime_error("UCI Arena Evidence Service CLI entrypoint is unavailable");
        }
        if (std::filesystem::exists(workspace) && !std::filesystem::is_directory(workspace)) {
            throw std::runtime_error("UCI Arena Evidence Service workspace is not a directory");
        }
        data["display_name"] =
            parameters.value("display_name", "UCI Arena Evidence Service");
        data["node_executable"] = std::filesystem::absolute(node).string();
        data["cli_entrypoint"] = std::filesystem::absolute(entrypoint).string();
        data["workspace"] = std::filesystem::absolute(workspace).string();
        data["integration_state"] = "configured";
        data["desired_state"] =
            parameters.value("startup_installed", false) ? "running" : "stopped";
        data["observed_state"] = "stopped";
        data["transport"] = "process";
        data["capabilities"] = evidence_service_profile().at("capabilities");
        if (parameters.value("startup_installed", false)) {
            data["startup_control"] = "provider_owned_per_user";
            for (const auto* capability : {"start", "stop", "restart"}) {
                const auto iterator =
                    std::find(data["capabilities"].begin(),
                              data["capabilities"].end(), capability);
                if (iterator != data["capabilities"].end()) {
                    data["capabilities"].erase(iterator);
                }
            }
        }
        data["health"] = {
            {"state", "unknown"},
            {"reason", "status has not been probed"},
            {"stale_after_seconds", 300},
        };
        return data;
    }
    throw std::runtime_error("unknown managed service profile: " + profile_id);
}

json ServiceCatalog::installed_registrations(
    const std::filesystem::path& manager_data_root) {
    const fs::path receipts_root = manager_data_root / "receipts";
    json services = json::array();
    json errors = json::array();
    if (!fs::is_directory(receipts_root)) {
        return {
            {"schema", "arena_installed_service_registrations_v1"},
            {"services", std::move(services)},
            {"errors", std::move(errors)},
        };
    }

    std::optional<InstalledComponent> node_runtime;
    try {
        node_runtime =
            installed_component(receipts_root, "node_runtime.private");
    } catch (const std::exception& error) {
        errors.push_back(
            {{"component_id", "node_runtime.private"},
             {"error", error.what()}});
    }
    for (const auto& [component_id, profile_id, display_name] :
         std::array{
             std::array<std::string, 3>{
                 "book_forge.service", "book_forge", "Book Forge"},
             std::array<std::string, 3>{
                 "uci_arena_evidence.service",
                 "uci_arena_evidence",
                 "UCI Arena Evidence Service"}}) {
        if (!fs::is_regular_file(receipts_root / (component_id + ".json"))) {
            continue;
        }
        try {
            if (!node_runtime.has_value()) {
                throw std::runtime_error(
                    "private Node runtime is unavailable");
            }
            const InstalledComponent service =
                installed_component(receipts_root, component_id);
            if (!service.receipt.contains("data_paths") ||
                !service.receipt.at("data_paths").is_array() ||
                service.receipt.at("data_paths").empty()) {
                throw std::runtime_error(
                    "installed service receipt has no workspace");
            }
            const fs::path workspace =
                service.receipt.at("data_paths").at(0).get<std::string>();
            if (!fs::is_directory(workspace)) {
                throw std::runtime_error(
                    "installed service workspace is unavailable");
            }
            json registration{
                {"profile_id", profile_id},
                {"display_name", display_name},
                {"node_executable",
                 verified_entrypoint(*node_runtime, "node").string()},
                {"cli_entrypoint",
                 verified_entrypoint(service, "cli").string()},
                {"workspace", workspace.string()},
                {"installed_component_id", component_id},
                {"installed_component_version",
                 service.receipt.at("component_version")},
                {"installed_artifact_sha256",
                 service.receipt.at("artifact_sha256")},
                {"runtime_component_id", "node_runtime.private"},
                {"runtime_artifact_sha256",
                 node_runtime->receipt.at("artifact_sha256")},
                {"startup_installed",
                 service.receipt.value("startup", json::object())
                     .value("installed", false)},
            };
            const std::string configuration_path =
                service.receipt.value("configuration_path", "");
            if (!configuration_path.empty()) {
                const fs::path configuration = configuration_path;
                if (!within(workspace, configuration) ||
                    !fs::is_regular_file(configuration) ||
                    fs::is_symlink(configuration)) {
                    throw std::runtime_error(
                        "installed service configuration is unavailable or unsafe");
                }
                registration["config_path"] =
                    fs::absolute(configuration).string();
            }
            services.push_back(normalize_registration(registration));
        } catch (const std::exception& error) {
            errors.push_back(
                {{"component_id", component_id}, {"error", error.what()}});
        }
    }
    return {
        {"schema", "arena_installed_service_registrations_v1"},
        {"services", std::move(services)},
        {"errors", std::move(errors)},
    };
}

json ServiceCatalog::component_inventory(
    const fs::path& manager_data_root) {
    const fs::path receipts_root = manager_data_root / "receipts";
    const std::set<std::string> downloadable =
        retained_catalog_components(manager_data_root);
    json components = json::array();
    for (const auto& [component_id, display_name, optional] :
         std::array{
             std::tuple{"uci_arena.engine", "UCI Arena Engine", false},
             std::tuple{"stockfish.engine", "Stockfish", false},
             std::tuple{"node_runtime.private", "Private Node runtime", false},
             std::tuple{"book_forge.service", "Book Forge", false},
             std::tuple{"opening_book.standard", "Opening book", false},
             std::tuple{"uci_arena_evidence.service",
                        "UCI Arena Evidence Service", false},
             std::tuple{"lichess.adapter", "Lichess Bot adapter", true}}) {
        json row{
            {"component_id", component_id},
            {"display_name", display_name},
            {"optional", optional},
            {"package_state", "missing"},
            {"downloadable", downloadable.contains(component_id)},
        };
        const fs::path receipt_path =
            receipts_root / (std::string(component_id) + ".json");
        if (fs::is_regular_file(receipt_path)) {
            try {
                const InstalledComponent component =
                    installed_component(receipts_root, component_id, true);
                row["package_state"] = "installed";
                row["installation_state"] =
                    component.receipt.value("state", "");
                row["component_version"] =
                    component.receipt.value("component_version", "");
                row["startup_installed"] =
                    component.receipt.value("startup", json::object())
                        .value("installed", false);
                if (component.manifest.contains("default_model") &&
                    component.manifest.at("default_model").is_object()) {
                    row["default_model"] =
                        component.manifest.at("default_model");
                }
            } catch (const std::exception& error) {
                row["package_state"] = "broken";
                row["issue"] = error.what();
            }
        }
        components.push_back(std::move(row));
    }
    return {
        {"schema", "arena_component_inventory_v1"},
        {"schema_version", 1},
        {"retained_catalog_available", !downloadable.empty()},
        {"components", std::move(components)},
    };
}

json ServiceCatalog::bot_profiles() {
    return json::array({
        {
            {"profile_id", "lichess_bot"},
            {"display_name", "Lichess Bot"},
            {"adapter_component_id", "lichess.adapter"},
            {"optional", true},
            {"capabilities",
             {"account_probe", "startup_status", "enable_startup",
              "disable_startup",
               "scheduler", "challenge_policy"}},
        },
    });
}

json ServiceCatalog::discover_bot(
    const fs::path& manager_data_root) {
    const fs::path receipts_root = manager_data_root / "receipts";
    json issues = json::array();
    json readiness = json::object();
    json candidate{
        {"candidate_id", "botcand-lichess-local"},
        {"profile_id", "lichess_bot"},
        {"display_name", "Lichess Bot"},
        {"adapter_component_id", "lichess.adapter"},
        {"package_state", "missing"},
        {"token_reference_available", false},
        {"configuration_state", "missing"},
        {"startup_state", "disabled"},
        {"options", json::object()},
    };
    bool adapter_ready = false;
    bool configuration_ready = false;
    bool startup_installed = false;
    std::optional<fs::path> adapter_root;
    json configuration = json::object();
    try {
        const InstalledComponent adapter =
            installed_component(receipts_root, "lichess.adapter", true);
        candidate["package_state"] = "installed";
        candidate["component_version"] =
            adapter.receipt.value("component_version", "");
        static_cast<void>(verified_entrypoint(adapter, "adapter"));
        adapter_root = adapter.active_root;
        adapter_ready = true;
        const std::string configuration_path =
            adapter.receipt.value("configuration_path", "");
        if (configuration_path.empty() ||
            !fs::is_regular_file(configuration_path) ||
            fs::is_symlink(configuration_path)) {
            issues.push_back({
                {"code", "bot_configuration_missing"},
                {"detail", "The installed adapter configuration is unavailable."},
            });
        } else {
            candidate["configuration_state"] = "configured";
            try {
                configuration = read_json_file(configuration_path);
                configuration_ready = true;
                candidate["options"] = {
                    {"capacity",
                     configuration.value("capacity", json::object())},
                    {"challenges",
                     configuration.value("challenges", json::object())},
                    {"outbound_challenge",
                     configuration.value(
                         "outbound_challenge", json::object())},
                    {"scheduler",
                     configuration.value("scheduler", json::object())},
                };
            } catch (const std::exception&) {
                issues.push_back({
                    {"code", "bot_configuration_invalid"},
                    {"detail", "The installed adapter configuration is invalid."},
                });
            }
        }
        startup_installed =
            adapter.receipt.value("startup", json::object())
                .value("installed", false);
        candidate["startup_state"] =
            startup_installed ? "installed" : "disabled";
    } catch (const std::exception&) {
    }
    project_bot_prerequisite(
        readiness,
        issues,
        "verified_adapter",
        adapter_ready,
        "bot_adapter_unavailable",
        "Install or repair the verified Lichess Bot adapter.");
    readiness["startup"] = startup_installed ? "enabled" : "disabled";

    bool runtime_ready = false;
    try {
        static_cast<void>(
            installed_component(receipts_root, "node_runtime.private"));
        runtime_ready = true;
    } catch (const std::exception& error) {
        issues.push_back({
            {"code", "bot_runtime_unavailable"},
            {"detail", error.what()},
        });
    }
    readiness["adapter_runtime"] = runtime_ready ? "ready" : "missing";

    bool engine_ready = false;
    bool model_ready = false;
    try {
        const InstalledComponent engine =
            installed_component(receipts_root, "uci_arena.engine");
        static_cast<void>(verified_entrypoint(engine, "uci"));
        engine_ready = true;
        if (engine.manifest.contains("default_model") &&
            engine.manifest.at("default_model").is_object()) {
            const json& model = engine.manifest.at("default_model");
            if (!model.value("model_id", "").empty() &&
                !model.value("display_name", "").empty() &&
                !model.value("input_adapter_id", "").empty()) {
                model_ready = true;
                candidate["default_model"] = model;
            }
        }
    } catch (const std::exception&) {
    }
    project_bot_prerequisite(
        readiness,
        issues,
        "verified_uci_arena_engine",
        engine_ready,
        "bot_engine_unavailable",
        "Install or repair the verified UCI Arena Engine component.");
    project_bot_prerequisite(
        readiness,
        issues,
        "active_model",
        model_ready,
        "bot_model_unavailable",
        "The installed UCI Arena Engine does not expose a verified default model.");

    bool opening_book_ready = false;
    try {
        static_cast<void>(
            installed_component(receipts_root, "opening_book.standard"));
        opening_book_ready = true;
    } catch (const std::exception&) {
    }
    project_bot_prerequisite(
        readiness,
        issues,
        "opening_book",
        opening_book_ready,
        "bot_opening_book_unavailable",
        "Install or repair the signed standard opening-book component.");

    bool syzygy_ready = false;
    try {
        static_cast<void>(
            installed_component(receipts_root, "syzygy.wdl.3-4-5"));
        syzygy_ready = true;
    } catch (const std::exception&) {
        syzygy_ready =
            saved_locator_reference(
                manager_data_root, "syzygy", true).has_value();
    }
    project_bot_prerequisite(
        readiness,
        issues,
        "syzygy",
        syzygy_ready,
        "bot_syzygy_unavailable",
        "Install bounded Syzygy WDL data or select a verified local Syzygy directory.");

    const auto timing =
        saved_locator_reference(manager_data_root, "timing_profile");
    const auto single_game =
        saved_locator_reference(
            manager_data_root, "lichess_single_game_profile");
    const auto capacity =
        saved_locator_reference(
            manager_data_root, "lichess_capacity_profile");
    const auto transport =
        saved_locator_reference(
            manager_data_root, "lichess_transport_profile");
    const auto configured_path_matches = [&configuration, configuration_ready](
                                             const std::vector<std::string>& fields,
                                             const std::optional<fs::path>& locator) {
        if (!configuration_ready || !locator.has_value()) {
            return false;
        }
        const json* current = &configuration;
        for (const auto& field : fields) {
            if (!current->is_object() || !current->contains(field)) {
                return false;
            }
            current = &current->at(field);
        }
        if (!current->is_string()) {
            return false;
        }
        return fs::absolute(current->get<std::string>()).lexically_normal() ==
               fs::absolute(*locator).lexically_normal();
    };
    project_bot_prerequisite(
        readiness,
        issues,
        "timing_profile",
        configured_path_matches(
            {"engine", "time_profile"}, timing),
        "bot_timing_profile_unavailable",
        "Import an exact qualified timing profile for the installed engine and model.");
    project_bot_prerequisite(
        readiness,
        issues,
        "single_game_profile",
        configured_path_matches(
            {"engine", "single_game_profile"}, single_game),
        "bot_single_game_profile_unavailable",
        "Import an exact qualified Lichess single-game profile.");
    project_bot_prerequisite(
        readiness,
        issues,
        "capacity_profile",
        configured_path_matches(
            {"capacity", "capacity_profile"}, capacity),
        "bot_capacity_profile_unavailable",
        "Import a qualified Lichess capacity profile for this runtime.");
    project_bot_prerequisite(
        readiness,
        issues,
        "transport_profile",
        configured_path_matches(
            {"timing", "transport_profile_path"}, transport),
        "bot_transport_profile_unavailable",
        "Import a qualified Lichess transport profile.");

    const auto token_reference = saved_locator_reference(
        manager_data_root, "lichess_token_path");
    const bool token_inside_adapter =
        token_reference.has_value() && adapter_root.has_value() &&
        same_or_within(*adapter_root, *token_reference);
    const bool token_ready =
        token_reference.has_value() && !token_inside_adapter;
    if (token_ready) {
        candidate["token_reference_available"] = true;
    }
    project_bot_prerequisite(
        readiness,
        issues,
        "token_reference",
        token_ready,
        token_inside_adapter
            ? "bot_token_reference_not_external"
            : "bot_token_reference_missing",
        token_inside_adapter
            ? "Move the Lichess token file outside the installed adapter and select it again."
            : "Select a Lichess token-file reference before probing the account.");
    readiness["bot_account_probe"] = "not_verified";
    candidate["readiness"] = readiness;
    candidate["issues"] = issues;
    candidate["state"] =
        issues.empty() ? "ready_to_probe" : "blocked";
    return {
        {"schema", "arena_bot_candidates_v1"},
        {"schema_version", 1},
        {"candidates", json::array({std::move(candidate)})},
    };
}

json ServiceCatalog::probe_bot(
    const fs::path& manager_data_root,
    const ProcessSupervisor& process_supervisor) {
    return probe_bot(
        manager_data_root,
        [&process_supervisor](
            const fs::path& executable,
            const fs::path& working_directory,
            const std::vector<std::string>& arguments,
            const std::chrono::milliseconds timeout) {
            return process_supervisor.run_json(
                executable, working_directory, arguments, timeout);
        });
}

json ServiceCatalog::probe_bot(
    const fs::path& manager_data_root,
    const BotCommandRunner& command_runner) {
    const fs::path receipts_root = manager_data_root / "receipts";
    const InstalledComponent adapter =
        installed_component(receipts_root, "lichess.adapter", true);
    const InstalledComponent node =
        installed_component(receipts_root, "node_runtime.private");
    const fs::path token_path =
        saved_locator_reference(manager_data_root, "lichess_token_path")
            .value_or(fs::path{});
    if (token_path.empty()) {
        throw std::runtime_error(
            "a valid Lichess token-file reference is required");
    }
    if (same_or_within(adapter.active_root, token_path)) {
        throw std::runtime_error(
            "the Lichess token file must be outside the installed adapter");
    }
    const std::string configuration_path =
        adapter.receipt.value("configuration_path", "");
    if (configuration_path.empty() ||
        !fs::is_regular_file(configuration_path) ||
        fs::is_symlink(configuration_path)) {
        throw std::runtime_error(
            "the installed Lichess adapter configuration is unavailable");
    }
    const fs::path adapter_entrypoint =
        verified_entrypoint(adapter, "adapter");
    const json result = command_runner(
        verified_entrypoint(node, "node"),
        adapter.active_root,
        {
            adapter_entrypoint.string(),
            "probe",
            "--config",
            configuration_path,
            "--token-file",
            token_path.string(),
        },
        std::chrono::seconds(30));
    if (result.value("status", "") != "ready" ||
        !result.value("is_bot", false) ||
        !result.contains("account") ||
        !result.at("account").is_string() ||
        !valid_lichess_account_name(
            result.at("account").get_ref<const std::string&>()) ||
        (result.contains("games") &&
         !result.at("games").is_number_unsigned())) {
        throw std::runtime_error(
            "the adapter did not return a valid BOT account identity");
    }
    return {
        {"schema", "arena_bot_probe_result_v1"},
        {"schema_version", 1},
        {"state", "passed"},
        {"account_name", result.at("account")},
        {"is_bot", true},
        {"games", result.value("games", 0ULL)},
    };
}

json ServiceCatalog::control_bot(
    const fs::path& manager_data_root,
    const ProcessSupervisor& process_supervisor,
    const std::string& action) {
    return control_bot(
        manager_data_root,
        [&process_supervisor](
            const fs::path& executable,
            const fs::path& working_directory,
            const std::vector<std::string>& arguments,
            const std::chrono::milliseconds timeout) {
            return process_supervisor.run_json(
                executable, working_directory, arguments, timeout);
        },
        action);
}

json ServiceCatalog::control_bot(
    const fs::path& manager_data_root,
    const BotCommandRunner& command_runner,
    const std::string& action) {
    if (action != "status" && action != "enable_startup" &&
        action != "disable_startup") {
        throw std::invalid_argument("unsupported BOT account-control action");
    }
    const fs::path receipts_root = manager_data_root / "receipts";
    const InstalledComponent adapter =
        installed_component(receipts_root, "lichess.adapter", true);
    const InstalledComponent node =
        installed_component(receipts_root, "node_runtime.private");
    if (!adapter.receipt.contains("data_paths") ||
        !adapter.receipt.at("data_paths").is_array() ||
        adapter.receipt.at("data_paths").empty() ||
        !adapter.receipt.at("data_paths").at(0).is_string()) {
        throw std::runtime_error(
            "the installed Lichess adapter has no managed runtime directory");
    }
    const fs::path runtime_directory = fs::absolute(
        adapter.receipt.at("data_paths").at(0).get<std::string>())
                                               .lexically_normal();
    const fs::path adapter_entrypoint =
        verified_entrypoint(adapter, "adapter");
    const fs::path node_entrypoint = verified_entrypoint(node, "node");

    std::vector<std::string> arguments{
        adapter_entrypoint.string(),
        "startup",
    };
    std::optional<std::string> account_name;
    if (action == "enable_startup") {
        if (discover_bot(manager_data_root).at("candidates").at(0).at("state") !=
            "ready_to_probe") {
            throw std::runtime_error(
                "every BOT readiness prerequisite is required before startup");
        }
        const json probe = probe_bot(manager_data_root, command_runner);
        account_name = probe.at("account_name").get<std::string>();
        const std::string configuration_path =
            adapter.receipt.value("configuration_path", "");
        const fs::path token_path =
            saved_locator_reference(manager_data_root, "lichess_token_path")
                .value_or(fs::path{});
        if (configuration_path.empty() || token_path.empty()) {
            throw std::runtime_error(
                "BOT startup configuration is incomplete");
        }
        arguments.insert(
            arguments.end(),
            {
                "install",
                "--config",
                configuration_path,
                "--token-file",
                token_path.string(),
                "--expected-account",
                *account_name,
                "--runtime-dir",
                runtime_directory.string(),
            });
    } else {
        arguments.push_back(
            action == "disable_startup" ? "uninstall" : "status");
        if (action == "disable_startup") {
            arguments.insert(
                arguments.end(),
                {"--runtime-dir", runtime_directory.string()});
        }
    }

    const json result = command_runner(
        node_entrypoint,
        adapter.active_root,
        arguments,
        action == "enable_startup"
            ? std::chrono::minutes(5)
            : std::chrono::seconds(30));
    if (result.value("status", "") != "ready" ||
        !result.contains("startup") ||
        !result.at("startup").is_object() ||
        !result.at("startup").contains("installed") ||
        !result.at("startup").at("installed").is_boolean()) {
        throw std::runtime_error(
            "the provider returned invalid BOT startup status");
    }
    const json& startup = result.at("startup");
    const bool installed = startup.at("installed").get<bool>();
    if (action == "enable_startup" && !installed) {
        throw std::runtime_error(
            "the provider did not install BOT startup");
    }
    if (action == "disable_startup" && installed) {
        throw std::runtime_error(
            "the provider did not remove BOT startup");
    }
    const std::string provider_active =
        startup.value("active", std::string{});
    const std::string task_status =
        startup.value("taskStatus", std::string{});
    const bool active =
        provider_active == "active" ||
        task_status == "Running";
    json response{
        {"schema", "arena_bot_control_result_v1"},
        {"schema_version", 1},
        {"state", !installed ? "disabled" : active ? "active" : "enabled"},
        {"installed", installed},
        {"active", active},
    };
    if (account_name.has_value()) {
        response["account_name"] = *account_name;
    }
    return response;
}

}  // namespace arena::application
