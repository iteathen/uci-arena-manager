#include "arena/application/service_catalog.hpp"

#include <algorithm>
#include <charconv>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <optional>
#include <stdexcept>
#include <string_view>
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
        {"implementation_state", "in_development_external"},
        {"optional", true},
        {"description", "Versioned arena evidence, time-control curves, and provenance"},
        {"capabilities",
         {
             "status",
             "start",
             "stop",
             "restart",
             "pause",
             "resume",
             "ingest_result_evidence",
             "rebuild_curves",
             "validate_snapshot",
             "publish_snapshot",
             "list_snapshots",
         }},
        {"required_configuration", json::array()},
        {"activation_configuration", {"endpoint"}},
        {"discovery",
         {
             {"supported", true},
             {"manual_fallback", true},
             {"sources", {"process_environment_hints"}},
         }},
        {"planned_http_contract",
         {
             {"capabilities", "/api/v1/capabilities"},
             {"health", "/api/v1/health"},
             {"evidence", "/api/v1/evidence"},
             {"curves", "/api/v1/curves"},
             {"snapshots", "/api/v1/snapshots"},
             {"operations", "/api/v1/operations"},
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

bool is_loopback_endpoint(const std::string& endpoint) {
    std::string lower = endpoint;
    std::ranges::transform(lower, lower.begin(), [](const unsigned char value) {
        return static_cast<char>(std::tolower(value));
    });
    static constexpr const char* prefixes[] = {
        "http://127.0.0.1:",
        "https://127.0.0.1:",
        "http://localhost:",
        "https://localhost:",
        "http://[::1]:",
        "https://[::1]:",
    };
    for (const char* prefix : prefixes) {
        if (!lower.starts_with(prefix)) {
            continue;
        }
        const std::string_view remainder(lower.data() + std::char_traits<char>::length(prefix));
        const auto delimiter = remainder.find_first_of("/?#");
        const std::string_view port = remainder.substr(0, delimiter);
        unsigned int value = 0;
        const auto parsed =
            std::from_chars(port.data(), port.data() + port.size(), value);
        return !port.empty() && parsed.ec == std::errc{} &&
               parsed.ptr == port.data() + port.size() && value > 0 && value <= 65'535;
    }
    return false;
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
        manual_configuration = {"endpoint"};
        if (const auto endpoint = environment_value("UCI_ARENA_EVIDENCE_ENDPOINT")) {
            candidates.push_back(
                {
                    {"source", "process_environment_hint"},
                    {"complete", is_loopback_endpoint(*endpoint)},
                    {"configuration", {{"endpoint", *endpoint}}},
                    {"missing",
                     is_loopback_endpoint(*endpoint)
                         ? json::array()
                         : json::array({"valid_loopback_endpoint"})},
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
        data["display_name"] =
            parameters.value("display_name", "UCI Arena Evidence Service");
        const std::string endpoint = parameters.value("endpoint", "");
        if (!endpoint.empty() && !is_loopback_endpoint(endpoint)) {
            throw std::runtime_error(
                "UCI Arena Evidence Service endpoint must use a loopback HTTP URL in v1");
        }
        data["endpoint"] = endpoint.empty() ? json(nullptr) : json(endpoint);
        data["integration_state"] = endpoint.empty() ? "planned" : "configured";
        data["desired_state"] = "stopped";
        data["observed_state"] = "unavailable";
        data["transport"] = nullptr;
        data["capabilities"] = evidence_service_profile().at("capabilities");
        data["health"] = {
            {"state", "unavailable"},
            {"reason",
             endpoint.empty()
                 ? "optional UCI Arena Evidence Service endpoint is not configured"
                 : "configured endpoint awaits the versioned capability handshake"},
        };
        data["planned_http_contract"] =
            evidence_service_profile().at("planned_http_contract");
        return data;
    }
    throw std::runtime_error("unknown managed service profile: " + profile_id);
}

}  // namespace arena::application
