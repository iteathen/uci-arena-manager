#include "arena/application/service_catalog.hpp"

#include <filesystem>
#include <stdexcept>

namespace arena::application {
namespace {

using nlohmann::json;

json book_forge_profile() {
    return {
        {"profile_id", "book_forge"},
        {"display_name", "Book Forge"},
        {"implementation_state", "implemented_external"},
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

}  // namespace

json ServiceCatalog::profiles() {
    return json::array({book_forge_profile(), evidence_service_profile()});
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
        data["endpoint"] = endpoint.empty() ? json(nullptr) : json(endpoint);
        data["integration_state"] = endpoint.empty() ? "planned" : "configured";
        data["desired_state"] = "stopped";
        data["observed_state"] = "unavailable";
        data["transport"] = nullptr;
        data["capabilities"] = evidence_service_profile().at("capabilities");
        data["health"] = {
            {"state", "unavailable"},
            {"reason", "the external UCI Arena Evidence Service endpoint is not available yet"},
        };
        data["planned_http_contract"] =
            evidence_service_profile().at("planned_http_contract");
        return data;
    }
    throw std::runtime_error("unknown managed service profile: " + profile_id);
}

}  // namespace arena::application
