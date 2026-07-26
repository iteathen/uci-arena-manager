#include "arena/application/service_catalog.hpp"

#include <cstdlib>
#include <filesystem>
#include <optional>
#include <stdexcept>
#include <vector>

namespace arena::application {
namespace {

using nlohmann::json;

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

}  // namespace

json ServiceCatalog::profiles() {
    return json::array({book_forge_profile(), evidence_service_profile()});
}

json ServiceCatalog::discover(const std::string& profile_id) {
    json candidates = json::array();
    json manual_configuration = json::array();
    if (profile_id == "book_forge") {
        manual_configuration = {"node_executable", "cli_entrypoint", "workspace"};
        json configuration = json::object();
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
                    {"source", "environment_and_path"},
                    {"complete", missing.empty()},
                    {"configuration", configuration},
                    {"missing", missing},
                });
        }
    } else if (profile_id == "uci_arena_evidence") {
        manual_configuration = {"node_executable", "cli_entrypoint", "workspace"};
        json configuration = json::object();
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
                    {"source", "environment_and_path"},
                    {"complete", missing.empty()},
                    {"configuration", configuration},
                    {"missing", missing},
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

}  // namespace arena::application
