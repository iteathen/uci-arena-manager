#include "arena/application/service_catalog.hpp"

#include <sodium.h>

#include <array>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <set>
#include <stdexcept>
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
    const std::string& component_id) {
    const json receipt =
        read_json_file(receipts_root / (component_id + ".json"));
    if (receipt.value("schema", "") != "arena_install_receipt_v1" ||
        receipt.value("component_id", "") != component_id ||
        receipt.value("state", "") != "installed_managed") {
        throw std::runtime_error(
            component_id + " does not have an enabled managed receipt");
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
             {"schema", "flashfork_book_forge_status_v2"},
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

json lichess_bot_profile() {
    return {
        {"profile_id", "lichess_bot"},
        {"display_name", "Lichess BOT Adapter"},
        {"implementation_state", "implemented_external"},
        {"optional", true},
        {"description", "Durable Lichess account and event-stream scheduling adapter"},
        {"capabilities",
         {
             "status",
             "start",
             "stop",
             "restart",
             "pause",
             "resume",
             "probe",
             "owner_add",
             "owner_list",
             "owner_remove",
             "reserve",
             "reserve_cancel",
             "reserve_status",
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
             {"schema", "uci_arena_lichess_adapter_status_v1"},
             {"heartbeat_seconds", 15},
             {"stale_seconds", 300},
         }},
    };
}

}  // namespace

json ServiceCatalog::profiles() {
    return json::array({book_forge_profile(), evidence_service_profile(), lichess_bot_profile()});
}

json ServiceCatalog::discover(
    const std::string& profile_id,
    const std::filesystem::path& manager_data_root) {
    json candidates = json::array();
    json manual_configuration = json::array();
    if (profile_id == "lichess_bot") {
        manual_configuration = {"node_executable", "cli_entrypoint", "workspace"};
        json configuration = json::object();
        const auto saved = saved_service_locator(
            manager_data_root, "lichess_bot", "lichess.adapter");
        if (saved.cli_entrypoint) {
            configuration["cli_entrypoint"] =
                saved.cli_entrypoint->string();
        }
        if (const auto configured_node = environment_value("LICHESS_BOT_NODE")) {
            configuration["node_executable"] = *configured_node;
        } else if (const auto path_node = executable_on_path("node")) {
            configuration["node_executable"] = path_node->string();
        }
        if (const auto cli = environment_value("LICHESS_BOT_CLI")) {
            configuration["cli_entrypoint"] = *cli;
        }
        if (const auto workspace = environment_value("LICHESS_BOT_WORKSPACE")) {
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
    } else if (profile_id == "book_forge") {
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
    if (profile_id == "lichess_bot") {
        const auto node = required_path(parameters, "node_executable");
        const auto entrypoint = required_path(parameters, "cli_entrypoint");
        const auto workspace = required_path(parameters, "workspace");
        if (!std::filesystem::is_regular_file(node)) {
            throw std::runtime_error("Lichess Bot Node executable is unavailable");
        }
        if (!std::filesystem::is_regular_file(entrypoint)) {
            throw std::runtime_error("Lichess Bot CLI entrypoint is unavailable");
        }
        if (std::filesystem::exists(workspace) && !std::filesystem::is_directory(workspace)) {
            throw std::runtime_error("Lichess Bot workspace is not a directory");
        }
        data["display_name"] =
            parameters.value("display_name", "Lichess BOT Adapter");
        data["node_executable"] = std::filesystem::absolute(node).string();
        data["cli_entrypoint"] = std::filesystem::absolute(entrypoint).string();
        data["workspace"] = std::filesystem::absolute(workspace).string();
        data["integration_state"] = "configured";
        data["desired_state"] = "stopped";
        data["observed_state"] = "stopped";
        data["transport"] = "process";
        data["capabilities"] = lichess_bot_profile().at("capabilities");
        data["health"] = {
            {"state", "unknown"},
            {"reason", "status has not been probed"},
            {"stale_after_seconds", 300},
        };
        return data;
    }
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
        data["desired_state"] = "stopped";
        data["observed_state"] = "stopped";
        data["transport"] = "process";
        data["capabilities"] = book_forge_profile().at("capabilities");
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
        data["desired_state"] = "stopped";
        data["observed_state"] = "stopped";
        data["transport"] = "process";
        data["capabilities"] = evidence_service_profile().at("capabilities");
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
                 "UCI Arena Evidence Service"},
             std::array<std::string, 3>{
                 "lichess.adapter",
                 "lichess_bot",
                 "Lichess BOT Adapter"}}) {
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

}  // namespace arena::application
