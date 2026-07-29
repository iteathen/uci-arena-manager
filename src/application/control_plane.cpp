#include "arena/application/control_plane.hpp"

#include "arena/adapters/uci_process.hpp"
#include "arena/application/installer/setup.hpp"
#include "arena/application/service_catalog.hpp"
#include "arena/domain/match_planner.hpp"
#include "arena/domain/uuid.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <filesystem>
#include <set>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace arena::application {
namespace {

using nlohmann::json;

std::string required_string(const json& object, const char* name) {
    if (!object.contains(name) || !object.at(name).is_string() ||
        object.at(name).get<std::string>().empty()) {
        throw CommandFailure(
            "invalid_parameter",
            std::string("required string parameter is missing: ") + name,
            400);
    }
    return object.at(name).get<std::string>();
}

std::vector<std::string> string_array(const json& object, const char* name) {
    if (!object.contains(name)) {
        return {};
    }
    if (!object.at(name).is_array()) {
        throw CommandFailure(
            "invalid_parameter",
            std::string(name) + " must be an array of strings",
            400);
    }
    std::vector<std::string> values;
    for (const auto& value : object.at(name)) {
        if (!value.is_string()) {
            throw CommandFailure(
                "invalid_parameter",
                std::string(name) + " must contain only strings",
                400);
        }
        values.push_back(value.get<std::string>());
    }
    return values;
}

json labels_from(const json& parameters) {
    if (!parameters.contains("labels")) {
        return json::object();
    }
    if (!parameters.at("labels").is_object()) {
        throw CommandFailure("invalid_parameter", "labels must be an object", 400);
    }
    for (const auto& [key, value] : parameters.at("labels").items()) {
        (void)key;
        if (!value.is_string()) {
            throw CommandFailure("invalid_parameter", "label values must be strings", 400);
        }
    }
    return parameters.at("labels");
}

json uci_option_json(const domain::UciOption& option) {
    return {
        {"name", option.name},
        {"type", option.type},
        {"default", option.default_value},
        {"minimum", option.minimum},
        {"maximum", option.maximum},
        {"variants", option.variants},
    };
}

json pairing_json(const domain::GamePairing& pairing) {
    return {
        {"sequence", pairing.sequence},
        {"white_engine_id", pairing.white_engine_id},
        {"black_engine_id", pairing.black_engine_id},
        {"round", pairing.round},
        {"board", pairing.board},
        {"opening_index", pairing.opening_index},
    };
}

bool sensitive_key(std::string key) {
    std::ranges::transform(key, key.begin(), [](const unsigned char value) {
        return static_cast<char>(std::tolower(value));
    });
    return key.find("token") != std::string::npos ||
           key.find("secret") != std::string::npos ||
           key.find("password") != std::string::npos ||
           key.find("authorization") != std::string::npos ||
           key.find("credential") != std::string::npos;
}

json redact_external_status(const json& value) {
    if (value.is_array()) {
        json result = json::array();
        for (const auto& item : value) {
            result.push_back(redact_external_status(item));
        }
        return result;
    }
    if (!value.is_object()) {
        return value;
    }
    json result = json::object();
    for (const auto& [key, child] : value.items()) {
        result[key] = sensitive_key(key) ? json("[REDACTED]") :
            redact_external_status(child);
    }
    return result;
}

json bounded_redacted_external_value(
    const json& value,
    const std::size_t maximum_bytes,
    const char* label) {
    if (value.dump().size() > maximum_bytes) {
        throw std::runtime_error(
            std::string(label) + " exceeds the integration size limit");
    }
    return redact_external_status(value);
}

bool lowercase_hex(const std::string_view value, const std::size_t size) {
    return value.size() == size &&
           std::ranges::all_of(value, [](const unsigned char character) {
               return std::isdigit(character) != 0 ||
                      (character >= 'a' && character <= 'f');
           });
}

bool compatible_manager_api_version(const std::string_view version) {
    return version.size() > 2 && version.starts_with("1.") &&
           std::ranges::all_of(
               version.substr(2),
               [](const unsigned char character) {
                   return std::isdigit(character) != 0;
               });
}

bool uuid_v4(const std::string_view value) {
    if (value.size() != 36 || value[8] != '-' || value[13] != '-' ||
        value[18] != '-' || value[23] != '-' || value[14] != '4' ||
        (value[19] != '8' && value[19] != '9' &&
         value[19] != 'a' && value[19] != 'b')) {
        return false;
    }
    for (std::size_t index = 0; index < value.size(); ++index) {
        if (index == 8 || index == 13 || index == 18 || index == 23) {
            continue;
        }
        const unsigned char character =
            static_cast<unsigned char>(value[index]);
        if (std::isdigit(character) == 0 &&
            (character < 'a' || character > 'f')) {
            return false;
        }
    }
    return true;
}

std::set<std::string> string_set(
    const json& object,
    const char* field,
    const std::size_t maximum_values) {
    if (!object.contains(field) || !object.at(field).is_array() ||
        object.at(field).size() > maximum_values) {
        throw std::runtime_error(
            std::string("service API field is not a bounded string array: ") +
            field);
    }
    std::set<std::string> result;
    for (const auto& value : object.at(field)) {
        if (!value.is_string() || value.get_ref<const std::string&>().empty() ||
            value.get_ref<const std::string&>().size() > 128 ||
            !result.insert(value.get<std::string>()).second) {
            throw std::runtime_error(
                std::string("service API string array is malformed: ") +
                field);
        }
    }
    return result;
}

json manager_service_api_declaration(const json& data) {
    if (!data.contains("manager_integration_api") ||
        !data.at("manager_integration_api").is_object()) {
        throw std::runtime_error(
            "service has no compatible Manager API declaration");
    }
    const json declaration = data.at("manager_integration_api");
    if (declaration.value("schema", "") !=
            "uci_arena_manager_service_api_v1" ||
        !declaration.contains("version") ||
        !declaration.at("version").is_string() ||
        !compatible_manager_api_version(
            declaration.at("version").get_ref<const std::string&>()) ||
        declaration.value("transport", "") != "cli_json" ||
        declaration.value("entrypoint", "") != "cli" ||
        declaration.value("arguments", json::array()) !=
            json::array({"api"}) ||
        declaration.value("output_schema", "") !=
            "uci_arena_service_output_v1") {
        throw std::runtime_error(
            "service Manager API declaration is incompatible");
    }
    const std::set<std::string> required{
        "config_validate",
        "describe",
        "outputs_inspect",
        "outputs_list",
        "status",
    };
    if (string_set(declaration, "operations", 5) != required) {
        throw std::runtime_error(
            "service Manager API declaration is missing required operations");
    }
    return declaration;
}

json manager_service_api_payload(
    const json& response,
    const std::string_view operation,
    const std::string_view component_id,
    const std::string_view component_version) {
    if (!response.is_object() ||
        response.value("schema", "") !=
            "uci_arena_manager_service_api_v1" ||
        !response.contains("api_version") ||
        !response.at("api_version").is_string() ||
        !compatible_manager_api_version(
            response.at("api_version").get_ref<const std::string&>()) ||
        !response.contains("component_version") ||
        !response.at("component_version").is_string() ||
        response.at("component_version")
            .get_ref<const std::string&>()
            .empty() ||
        response.value("component_id", "") != component_id ||
        (!component_version.empty() &&
         response.value("component_version", "") != component_version) ||
        response.value("operation", "") != operation ||
        response.value("ok", false) != true ||
        !response.contains("payload") ||
        !response.at("payload").is_object()) {
        throw std::runtime_error(
            "service Manager API envelope is incompatible");
    }
    if (response.dump().size() > 16ULL * 1024ULL * 1024ULL) {
        throw std::runtime_error(
            "service Manager API response exceeds the integration size limit");
    }
    return response.at("payload");
}

void validate_service_output_base(
    const json& output,
    const std::set<std::string>& allowed_kinds) {
    if (!output.is_object() ||
        output.value("schema", "") != "uci_arena_service_output_v1" ||
        !output.contains("output_id") ||
        !output.at("output_id").is_string() ||
        output.at("output_id").get_ref<const std::string&>().empty() ||
        output.at("output_id").get_ref<const std::string&>().size() > 256 ||
        !output.contains("output_kind") ||
        !output.at("output_kind").is_string() ||
        !allowed_kinds.contains(output.at("output_kind").get<std::string>()) ||
        !output.contains("created_at") ||
        !output.at("created_at").is_string() ||
        !output.contains("state") || !output.at("state").is_object() ||
        !output.contains("authority") ||
        !output.at("authority").is_object() ||
        !output.contains("compatibility") ||
        !output.at("compatibility").is_object()) {
        throw std::runtime_error(
            "service output record violates the common API schema");
    }
    if (output.dump().size() > 1024ULL * 1024ULL) {
        throw std::runtime_error(
            "service output record exceeds the integration size limit");
    }
}

}  // namespace

CommandFailure::CommandFailure(std::string code, std::string detail, int http_status)
    : std::runtime_error(std::move(detail)), code_(std::move(code)), http_status_(http_status) {}

const std::string& CommandFailure::code() const noexcept { return code_; }
int CommandFailure::http_status() const noexcept { return http_status_; }

ControlPlane::ControlPlane(std::shared_ptr<ports::Store> store)
    : store_(std::move(store)), started_at_(domain::utc_now()) {}

void ControlPlane::initialize() { store_->initialize(); }

void ControlPlane::reconcile_installed_services(
    const std::filesystem::path& manager_data_root) {
    manager_data_root_ = std::filesystem::absolute(manager_data_root).lexically_normal();
    try {
        const json discovered =
            ServiceCatalog::installed_registrations(manager_data_root);
        json changes = json::array();
        json errors = discovered.at("errors");
        std::vector<std::string> refresh_ids;
        for (const auto& registration : discovered.at("services")) {
            const std::string component_id =
                registration.at("installed_component_id").get<std::string>();
            std::optional<ports::ResourceRecord> existing;
            for (const auto& resource :
                 store_->list_resources("managed_service", 10'000, 0)) {
                try {
                    const json data = json::parse(resource.data_json);
                    if (data.value("installed_component_id", "") == component_id) {
                        existing = resource;
                        break;
                    }
                } catch (const json::exception&) {
                }
            }
            if (existing.has_value()) {
                const json current = json::parse(existing->data_json);
                if (current.value("installed_artifact_sha256", "") ==
                        registration.value("installed_artifact_sha256", "") &&
                    current.value("runtime_artifact_sha256", "") ==
                        registration.value("runtime_artifact_sha256", "") &&
                    current.value("desired_state", "") ==
                        registration.value("desired_state", "") &&
                    current.value("config_path", "") ==
                        registration.value("config_path", "")) {
                    changes.push_back(
                        {{"component_id", component_id}, {"state", "current"}});
                    refresh_ids.push_back(existing->id);
                    continue;
                }
            }

            const std::string now = domain::utc_now();
            const std::string command_id = domain::uuid_v7();
            ports::ResourceRecord resource{
                existing.has_value() ? existing->id : domain::uuid_v7(),
                "managed_service",
                "local",
                existing.has_value() ? existing->revision + 1 : 1,
                existing.has_value() ? existing->created_at : now,
                now,
                R"({"managed_by":"arena-setup"})",
                registration.dump(),
            };
            const json result{
                {"schema", "arena_installed_service_reconciliation_result_v1"},
                {"component_id", component_id},
                {"resource_id", resource.id},
                {"revision", resource.revision},
            };
            ports::CommandRecord command{
                command_id,
                domain::uuid_v7(),
                "installer-reconcile:" + command_id,
                existing.has_value() ? "service.reconcile_update" :
                                       "service.reconcile_create",
                "completed",
                result.dump(),
                now,
            };
            ports::EventRecord event{
                0,
                domain::uuid_v7(),
                now,
                "state",
                existing.has_value() ? "managed_service.reconciled" :
                                       "managed_service.created",
                "managed_service",
                resource.id,
                resource.revision,
                command_id,
                json{
                    {"actor", "arena-setup"},
                    {"component_id", component_id},
                    {"artifact_sha256",
                     registration.at("installed_artifact_sha256")},
                    {"runtime_artifact_sha256",
                     registration.at("runtime_artifact_sha256")},
                }.dump(),
            };
            if (existing.has_value()) {
                store_->update_resource(
                    resource, existing->revision, command, std::move(event));
            } else {
                store_->create_resource(resource, command, std::move(event));
            }
            refresh_ids.push_back(resource.id);
            changes.push_back(
                {{"component_id", component_id},
                 {"state", existing.has_value() ? "updated" : "created"},
                 {"resource_id", resource.id}});
        }
        for (const auto& resource_id : refresh_ids) {
            try {
                const std::string command_id = domain::uuid_v7();
                static_cast<void>(submit(
                    {
                        {"schema_version", 1},
                        {"request_id", domain::uuid_v7()},
                        {"idempotency_key",
                         "installer-reconcile-status:" + command_id},
                        {"target",
                         {{"kind", "managed_service"},
                          {"id", resource_id}}},
                        {"action", "service.status"},
                        {"dry_run", false},
                        {"parameters", json::object()},
                    },
                    "installer-reconcile"));
                changes.push_back(
                    {{"resource_id", resource_id},
                     {"state", "status_refreshed"}});
            } catch (const std::exception& error) {
                errors.push_back(
                    {{"resource_id", resource_id},
                     {"error", error.what()}});
            }
        }
        installed_service_reconciliation_ = {
            {"schema", "arena_installed_service_reconciliation_v1"},
            {"state",
             errors.empty() ? "complete" : "complete_with_errors"},
            {"observed_at", domain::utc_now()},
            {"changes", std::move(changes)},
            {"errors", std::move(errors)},
        };
    } catch (const std::exception& error) {
        installed_service_reconciliation_ = {
            {"schema", "arena_installed_service_reconciliation_v1"},
            {"state", "failed"},
            {"observed_at", domain::utc_now()},
            {"changes", json::array()},
            {"errors",
             json::array(
                 {{{"code", "installed_service_reconciliation_failed"},
                    {"detail", error.what()}}})},
        };
    }
}

json ControlPlane::status() const {
    return {
        {"schema_version", 1},
        {"service", "uci-arena-manager"},
        {"version", "0.1.0"},
        {"host_id", "local"},
        {"started_at", started_at_},
        {"database", "ready"},
        {"counts", store_->resource_counts()},
        {"installed_service_reconciliation",
         installed_service_reconciliation_},
    };
}

json ControlPlane::validate_envelope(const json& request) {
    if (!request.is_object()) {
        throw CommandFailure("invalid_command", "command body must be a JSON object", 400);
    }
    if (request.value("schema_version", 0) != 1) {
        throw CommandFailure("unsupported_schema", "schema_version must be 1", 400);
    }
    const auto request_id = required_string(request, "request_id");
    if (!domain::is_uuid(request_id)) {
        throw CommandFailure("invalid_request_id", "request_id must be a UUID", 400);
    }
    const auto idempotency_key = required_string(request, "idempotency_key");
    if (idempotency_key.size() > 128) {
        throw CommandFailure("invalid_idempotency_key", "idempotency_key exceeds 128 bytes", 400);
    }
    required_string(request, "action");
    if (!request.contains("target") || !request.at("target").is_object()) {
        throw CommandFailure("invalid_target", "target must be an object", 400);
    }
    required_string(request.at("target"), "kind");
    if (!request.contains("dry_run") || !request.at("dry_run").is_boolean()) {
        throw CommandFailure("invalid_command", "dry_run must be a boolean", 400);
    }
    if (!request.contains("parameters") || !request.at("parameters").is_object()) {
        throw CommandFailure("invalid_command", "parameters must be an object", 400);
    }
    return request;
}

json ControlPlane::submit(const json& raw_request, const std::string& actor) {
    const json request = validate_envelope(raw_request);
    if (const auto existing = store_->command_result(request.at("idempotency_key")); existing) {
        return json::parse(*existing);
    }

    const std::string action = request.at("action");
    try {
        if (action == "engine.register") {
            return register_engine(request, actor);
        }
        if (action == "match.plan" || action == "tournament.plan") {
            return plan_match(request, actor);
        }
        if (action == "service.register" ||
            action == "health_probe.register") {
            return register_generic_resource(request, actor);
        }
        if (action == "resource.remove") {
            return remove_resource(request, actor);
        }
        if (action.starts_with("service.")) {
            return manage_service(request, actor);
        }
    } catch (const CommandFailure&) {
        throw;
    } catch (const std::exception& error) {
        throw CommandFailure("command_failed", error.what(), 409);
    }
    throw CommandFailure("unknown_action", "unsupported action: " + action, 400);
}

json ControlPlane::register_engine(const json& request, const std::string& actor) {
    const json& parameters = request.at("parameters");
    const std::filesystem::path executable = required_string(parameters, "executable");
    if (!std::filesystem::is_regular_file(executable)) {
        throw CommandFailure("engine_not_found", "engine executable is not a regular file", 422);
    }
    const std::filesystem::path working_directory = parameters.value(
        "working_directory",
        executable.parent_path().string());
    if (!std::filesystem::is_directory(working_directory)) {
        throw CommandFailure("working_directory_not_found", "working directory is invalid", 422);
    }
    const auto arguments = string_array(parameters, "arguments");
    const auto probe = adapters::UciProcess::probe(executable, working_directory, arguments);
    if (!probe.ok) {
        throw CommandFailure("uci_probe_failed", probe.error, 422);
    }
    const std::string display_name =
        parameters.value("display_name", std::string{}).empty()
            ? (probe.engine_name.empty() ? "UCI engine" : probe.engine_name)
            : parameters.at("display_name").get<std::string>();

    const std::string id = domain::uuid_v7();
    json options = json::array();
    for (const auto& option : probe.options) {
        options.push_back(uci_option_json(option));
    }
    json data = {
        {"display_name", display_name},
        {"executable", std::filesystem::absolute(executable).string()},
        {"working_directory", std::filesystem::absolute(working_directory).string()},
        {"arguments", arguments},
        {"option_overrides", parameters.value("option_overrides", json::object())},
        {"discovered_options", options},
        {"engine_name", probe.engine_name},
        {"engine_author", probe.engine_author},
        {"fingerprint", probe.fingerprint},
        {"probe_bestmove", probe.bestmove},
        {"probe_elapsed_ms", probe.elapsed_ms},
        {"dropped_observational_lines", probe.dropped_info_lines},
        {"desired_state", "stopped"},
        {"observed_state", "stopped"},
    };

    const std::string now = domain::utc_now();
    return complete_create(
        request,
        actor,
        ports::ResourceRecord{
            id,
            "engine",
            "local",
            1,
            now,
            now,
            labels_from(parameters).dump(),
            data.dump(),
        });
}

json ControlPlane::plan_match(const json& request, const std::string& actor) {
    const json& parameters = request.at("parameters");
    domain::MatchPlan plan;
    plan.id = domain::uuid_v7();
    plan.display_name = parameters.value("display_name", std::string{});
    const auto format = domain::tournament_format_from_string(
        parameters.value("format", std::string{"head_to_head"}));
    if (!format) {
        throw CommandFailure("invalid_format", "unsupported tournament format", 400);
    }
    plan.format = *format;
    const auto variant = domain::chess_variant_from_string(
        parameters.value("variant", std::string{"standard"}));
    if (!variant) {
        throw CommandFailure("invalid_variant", "variant must be standard or chess960", 400);
    }
    plan.variant = *variant;
    plan.engine_ids = string_array(parameters, "engine_ids");
    for (const auto& engine_id : plan.engine_ids) {
        if (!domain::is_uuid(engine_id) || !store_->get_resource("engine", engine_id)) {
            throw CommandFailure(
                "engine_not_registered",
                "match plan references an unknown engine: " + engine_id,
                422);
        }
    }
    plan.games_per_pair = parameters.value("games_per_pair", 2U);
    plan.rounds = parameters.value("rounds", 1U);
    plan.concurrency = parameters.value("concurrency", 1U);
    plan.seed = parameters.value("seed", 0ULL);
    if (parameters.contains("time_control")) {
        const auto& time = parameters.at("time_control");
        plan.time_control.initial_ms = time.value("initial_ms", 60'000ULL);
        plan.time_control.increment_ms = time.value("increment_ms", 600ULL);
        if (time.contains("moves_to_go") && !time.at("moves_to_go").is_null()) {
            plan.time_control.moves_to_go = time.at("moves_to_go").get<std::uint32_t>();
        }
    }
    if (parameters.contains("opening_suite") && !parameters.at("opening_suite").is_null()) {
        plan.opening_suite = parameters.at("opening_suite").get<std::string>();
    }
    if (parameters.contains("engine_option_overrides")) {
        plan.engine_option_overrides =
            parameters.at("engine_option_overrides")
                .get<std::map<std::string, std::map<std::string, std::string>>>();
    }

    try {
        plan = domain::MatchPlanner::normalize_and_schedule(std::move(plan));
    } catch (const domain::PlanError& error) {
        throw CommandFailure("invalid_match_plan", error.what(), 400);
    }

    json pairings = json::array();
    for (const auto& pairing : plan.pairings) {
        pairings.push_back(pairing_json(pairing));
    }
    json data = {
        {"display_name", plan.display_name},
        {"format", domain::to_string(plan.format)},
        {"variant", domain::to_string(plan.variant)},
        {"engine_ids", plan.engine_ids},
        {"engine_option_overrides", plan.engine_option_overrides},
        {"games_per_pair", plan.games_per_pair},
        {"rounds", plan.rounds},
        {"concurrency", plan.concurrency},
        {"seed", plan.seed},
        {"time_control",
         {
             {"initial_ms", plan.time_control.initial_ms},
             {"increment_ms", plan.time_control.increment_ms},
             {"moves_to_go", plan.time_control.moves_to_go},
         }},
        {"opening_suite", plan.opening_suite ? json(plan.opening_suite->string()) : json(nullptr)},
        {"pairings", pairings},
        {"lifecycle", "planned"},
    };

    const std::string kind =
        request.at("action") == "match.plan" ? "match_plan" : "tournament";
    const std::string now = domain::utc_now();
    return complete_create(
        request,
        actor,
        ports::ResourceRecord{
            plan.id,
            kind,
            "local",
            1,
            now,
            now,
            labels_from(parameters).dump(),
            data.dump(),
        });
}

json ControlPlane::register_generic_resource(const json& request, const std::string& actor) {
    const std::string action = request.at("action");
    const std::string kind =
        action == "service.register" ? "managed_service" : "health_probe";
    const json& parameters = request.at("parameters");
    json data;
    if (kind == "managed_service") {
        for (const auto* protected_field :
             {"installed_component_id",
              "installed_component_version",
              "installed_artifact_sha256",
              "runtime_component_id",
              "runtime_artifact_sha256",
              "manager_integration_api",
              "manager_integration_api_source"}) {
            if (parameters.contains(protected_field)) {
                throw CommandFailure(
                    "invalid_parameter",
                    std::string("installer-owned service field is forbidden: ") +
                        protected_field,
                    400);
            }
        }
        try {
            data = ServiceCatalog::normalize_registration(parameters);
        } catch (const std::exception& error) {
            throw CommandFailure("invalid_service_profile", error.what(), 422);
        }
    } else {
        required_string(parameters, "display_name");
        data = parameters;
        data.erase("labels");
        data["desired_state"] = "stopped";
        data["observed_state"] = "stopped";
    }
    const std::string now = domain::utc_now();
    return complete_create(
        request,
        actor,
        ports::ResourceRecord{
            domain::uuid_v7(),
            kind,
            "local",
            1,
            now,
            now,
            labels_from(parameters).dump(),
            data.dump(),
        });
}

json ControlPlane::service_profiles() const { return ServiceCatalog::profiles(); }

json ControlPlane::discover_service_profile(const std::string& profile_id) const {
    try {
        return ServiceCatalog::discover(profile_id, manager_data_root_);
    } catch (const std::exception& error) {
        throw CommandFailure("service_profile_not_found", error.what(), 404);
    }
}

json ControlPlane::component_inventory() const {
    return ServiceCatalog::component_inventory(manager_data_root_);
}

json ControlPlane::bot_profiles() const {
    return ServiceCatalog::bot_profiles();
}

json ControlPlane::bot_candidates() const {
    return ServiceCatalog::discover_bot(manager_data_root_);
}

json ControlPlane::save_bot_token_reference(
    const std::filesystem::path& path) const {
    try {
        return installer::SetupEngine::save_lichess_token_reference(
            manager_data_root_, path);
    } catch (const std::exception& error) {
        throw CommandFailure(
            "bot_token_reference_invalid", error.what(), 422);
    }
}

json ControlPlane::complete_create(
    const json& request,
    const std::string& actor,
    ports::ResourceRecord resource) {
    json result = {
        {"schema_version", 1},
        {"request_id", request.at("request_id")},
        {"command_id", domain::uuid_v7()},
        {"status", "completed"},
        {"dry_run", request.at("dry_run")},
        {"resource", resource_json(resource)},
        {"operation_id", nullptr},
        {"error", nullptr},
    };
    if (request.at("dry_run").get<bool>()) {
        return result;
    }

    const std::string now = domain::utc_now();
    ports::CommandRecord command{
        result.at("command_id"),
        request.at("request_id"),
        request.at("idempotency_key"),
        request.at("action"),
        "completed",
        result.dump(),
        now,
    };
    ports::EventRecord event{
        0,
        domain::uuid_v7(),
        now,
        "state",
        resource.kind + ".created",
        resource.kind,
        resource.id,
        resource.revision,
        result.at("command_id").get<std::string>(),
        json{
            {"actor", actor},
            {"action", request.at("action")},
            {"resource", resource_json(resource)},
        }.dump(),
    };
    store_->create_resource(resource, command, event);
    return result;
}

json ControlPlane::complete_update(
    const json& request,
    const std::string& actor,
    ports::ResourceRecord resource,
    const std::uint64_t expected_revision,
    const std::string& event_kind) {
    json result = {
        {"schema_version", 1},
        {"request_id", request.at("request_id")},
        {"command_id", domain::uuid_v7()},
        {"status", "completed"},
        {"dry_run", request.at("dry_run")},
        {"resource", resource_json(resource)},
        {"operation_id", nullptr},
        {"error", nullptr},
    };
    if (request.at("dry_run").get<bool>()) {
        return result;
    }
    const std::string now = domain::utc_now();
    ports::CommandRecord command{
        result.at("command_id"),
        request.at("request_id"),
        request.at("idempotency_key"),
        request.at("action"),
        "completed",
        result.dump(),
        now,
    };
    ports::EventRecord event{
        0,
        domain::uuid_v7(),
        now,
        "state",
        event_kind,
        resource.kind,
        resource.id,
        resource.revision,
        result.at("command_id").get<std::string>(),
        json{
            {"actor", actor},
            {"action", request.at("action")},
            {"resource", resource_json(resource)},
        }.dump(),
    };
    store_->update_resource(resource, expected_revision, command, event);
    return result;
}

json ControlPlane::manage_service(const json& request, const std::string& actor) {
    const auto& target = request.at("target");
    if (required_string(target, "kind") != "managed_service") {
        throw CommandFailure("invalid_target", "service action requires managed_service target", 400);
    }
    const std::string id = required_string(target, "id");
    const auto existing = store_->get_resource("managed_service", id);
    if (!existing) {
        throw CommandFailure("resource_not_found", "managed service does not exist", 404);
    }
    json data = json::parse(existing->data_json);
    const std::string profile = data.value("profile_id", "");
    const std::string action = request.at("action");
    if (action == "service.configure") {
        if (data.value("observed_state", "stopped") == "running" ||
            data.value("observed_state", "stopped") == "paused") {
            throw CommandFailure(
                "service_running",
                "stop the managed service before changing its locator",
                409);
        }
        const json previous_data = data;
        json parameters = request.at("parameters");
        for (const auto* protected_field :
             {"installed_component_id",
              "installed_component_version",
              "installed_artifact_sha256",
              "runtime_component_id",
              "runtime_artifact_sha256",
              "manager_integration_api",
              "manager_integration_api_source"}) {
            parameters.erase(protected_field);
        }
        parameters["profile_id"] = profile;
        parameters["display_name"] =
            parameters.value("display_name", data.value("display_name", profile));
        try {
            data = ServiceCatalog::normalize_registration(parameters);
        } catch (const std::exception& error) {
            throw CommandFailure("invalid_service_profile", error.what(), 422);
        }
        for (const auto* protected_field :
             {"installed_component_id",
              "installed_component_version",
              "installed_artifact_sha256",
              "runtime_component_id",
              "runtime_artifact_sha256",
              "manager_integration_api",
              "manager_integration_api_source"}) {
            if (previous_data.contains(protected_field)) {
                data[protected_field] = previous_data.at(protected_field);
            }
        }
        ports::ResourceRecord updated = *existing;
        updated.revision = existing->revision + 1;
        updated.updated_at = domain::utc_now();
        updated.data_json = data.dump();
        const std::uint64_t expected_revision =
            request.contains("expected_revision") && !request.at("expected_revision").is_null()
                ? request.at("expected_revision").get<std::uint64_t>()
                : existing->revision;
        return complete_update(
            request,
            actor,
            std::move(updated),
            expected_revision,
            "managed_service.configured");
    }
    if (profile != "book_forge" && profile != "uci_arena_evidence") {
        throw CommandFailure("unsupported_service_profile", "service profile has no runtime adapter", 409);
    }
    const bool evidence_service = profile == "uci_arena_evidence";
    const bool startup_owned =
        data.value("startup_installed", false) ||
        data.value("startup_control", "") == "provider_owned_per_user";
    if (startup_owned &&
        (action == "service.start" || action == "service.stop" ||
         action == "service.restart")) {
        throw CommandFailure(
            "startup_control_unavailable",
            "this per-user provider startup is externally owned; use pause, resume, or "
            "the provider startup adapter from the installed user session",
            409);
    }

    const std::filesystem::path node = data.at("node_executable").get<std::string>();
    const std::filesystem::path cli = data.at("cli_entrypoint").get<std::string>();
    const std::filesystem::path workspace = data.at("workspace").get<std::string>();
    std::vector<std::string> base{cli.string()};
    const auto add_locator_options = [&data, &workspace](std::vector<std::string>& arguments) {
        arguments.insert(arguments.end(), {"--workspace", workspace.string()});
        const std::string config_path = data.value("config_path", "");
        if (!config_path.empty()) {
            arguments.insert(arguments.end(), {"--config", config_path});
        }
    };
    if (action == "service.start") {
        auto arguments = base;
        arguments.push_back("daemon");
        add_locator_options(arguments);
        if (!request.at("dry_run").get<bool>()) {
            process_supervisor_.start(id, node, cli.parent_path(), arguments);
        }
        data["desired_state"] = "running";
        data["observed_state"] = "running";
        data["integration_state"] = "available";
    } else if (action == "service.stop") {
        if (!request.at("dry_run").get<bool>()) {
            process_supervisor_.stop(id);
        }
        data["desired_state"] = "stopped";
        data["observed_state"] = "stopped";
    } else if (action == "service.restart") {
        if (!request.at("dry_run").get<bool>()) {
            process_supervisor_.stop(id);
            auto arguments = base;
            arguments.push_back("daemon");
            add_locator_options(arguments);
            process_supervisor_.start(id, node, cli.parent_path(), arguments);
        }
        data["desired_state"] = "running";
        data["observed_state"] = "running";
        data["integration_state"] = "available";
    } else if (action == "service.pause" || action == "service.resume") {
        auto arguments = base;
        arguments.push_back(action == "service.pause" ? "pause" : "resume");
        add_locator_options(arguments);
        if (evidence_service && action == "service.pause") {
            arguments.insert(arguments.end(), {"--reason", "uci-arena-manager"});
        }
        if (!request.at("dry_run").get<bool>()) {
            data["last_control_result"] = process_supervisor_.run_json(
                node, cli.parent_path(), arguments);
        }
        data["desired_state"] = action == "service.pause" ? "paused" : "running";
        data["observed_state"] = action == "service.pause" ? "paused" :
            (startup_owned || process_supervisor_.running(id) ? "running" : "stopped");
    } else if (action == "service.status") {
        auto arguments = base;
        arguments.push_back("status");
        add_locator_options(arguments);
        arguments.push_back("--json");
        if (!request.at("dry_run").get<bool>()) {
            if (data.contains("manager_integration_api")) {
                try {
                    const json declaration =
                        manager_service_api_declaration(data);
                    const std::string expected_component_id =
                        evidence_service
                            ? "uci_arena_evidence.service"
                            : "book_forge.service";
                    std::string expected_component_version =
                        data.value("installed_component_version", "");
                    const auto invoke_api =
                        [&](const std::vector<std::string>& operation_arguments) {
                            auto api_arguments = base;
                            for (const auto& prefix :
                                 declaration.at("arguments")) {
                                api_arguments.push_back(
                                    prefix.get<std::string>());
                            }
                            api_arguments.insert(
                                api_arguments.end(),
                                operation_arguments.begin(),
                                operation_arguments.end());
                            add_locator_options(api_arguments);
                            return process_supervisor_.run_json(
                                node, cli.parent_path(), api_arguments);
                        };

                    const json describe_response =
                        invoke_api({"describe"});
                    if (expected_component_version.empty()) {
                        if (!describe_response.contains(
                                "component_version") ||
                            !describe_response.at(
                                "component_version").is_string() ||
                            describe_response.at("component_version")
                                    .get_ref<const std::string&>()
                                    .empty()) {
                            throw std::runtime_error(
                                "service API did not report a component version");
                        }
                        expected_component_version =
                            describe_response.at("component_version")
                                .get<std::string>();
                    }
                    const json describe = manager_service_api_payload(
                        describe_response,
                        "describe",
                        expected_component_id,
                        expected_component_version);
                    const std::string expected_status_schema =
                        evidence_service
                            ? "uci_arena_evidence_status_v1"
                            : "uci_arena_book_forge_status_v2";
                    const std::string expected_configuration_schema =
                        evidence_service
                            ? "uci_arena_evidence_service_config_v1"
                            : "uci_arena_book_forge_config_v1";
                    const std::set<std::string> required_operations{
                        "config_validate",
                        "describe",
                        "outputs_inspect",
                        "outputs_list",
                        "status",
                    };
                    const std::set<std::string> allowed_output_kinds =
                        evidence_service
                            ? std::set<std::string>{
                                  "evidence.dataset",
                                  "timing_profile.qualified"}
                            : std::set<std::string>{
                                  "opening_book.snapshot"};
                    if (describe.value("transport", "") != "cli_json" ||
                        describe.value("invocation", json::array()) !=
                            json::array({"api"}) ||
                        string_set(describe, "operations", 32) !=
                            required_operations ||
                        string_set(describe, "output_schemas", 8) !=
                            std::set<std::string>{
                                "uci_arena_service_output_v1"} ||
                        string_set(describe, "output_kinds", 16) !=
                            allowed_output_kinds ||
                        describe.value("configuration_schema", "") !=
                            expected_configuration_schema ||
                        describe.value("status_schema", "") !=
                            expected_status_schema) {
                        throw std::runtime_error(
                            "service API description is incompatible");
                    }

                    const json status_payload =
                        manager_service_api_payload(
                            invoke_api({"status"}),
                            "status",
                            expected_component_id,
                            expected_component_version)
                            .at("status");
                    if (!status_payload.is_object() ||
                        status_payload.value("schema", "") !=
                            expected_status_schema) {
                        throw std::runtime_error(
                            "service API status schema is incompatible");
                    }

                    std::vector<std::string> output_arguments{
                        "outputs", "list"};
                    const std::string previous_cursor =
                        data.value("output_cursor", "");
                    if (!previous_cursor.empty()) {
                        if (!lowercase_hex(previous_cursor, 64)) {
                            throw std::runtime_error(
                                "stored service output cursor is malformed");
                        }
                        output_arguments.insert(
                            output_arguments.end(),
                            {"--cursor", previous_cursor});
                    }
                    const json listed = manager_service_api_payload(
                        invoke_api(output_arguments),
                        "outputs_list",
                        expected_component_id,
                        expected_component_version);
                    if (!listed.contains("cursor") ||
                        !listed.at("cursor").is_string() ||
                        !lowercase_hex(
                            listed.at("cursor").get_ref<
                                const std::string&>(),
                            64) ||
                        !listed.contains("unchanged") ||
                        !listed.at("unchanged").is_boolean() ||
                        !listed.contains("outputs") ||
                        !listed.at("outputs").is_array() ||
                        listed.at("outputs").size() > 10'000) {
                        throw std::runtime_error(
                            "service API output listing is malformed");
                    }
                    const bool unchanged =
                        listed.at("unchanged").get<bool>();
                    if (unchanged &&
                        (previous_cursor.empty() ||
                         listed.at("cursor") != previous_cursor ||
                         !listed.at("outputs").empty() ||
                         !data.contains("output_integration"))) {
                        throw std::runtime_error(
                            "service API unchanged output cursor is inconsistent");
                    }

                    if (!unchanged) {
                        if (evidence_service) {
                            json dataset_projection = json::array();
                            json profile_projection = json::array();
                            json selected_profile = nullptr;
                            for (const auto& output :
                                 listed.at("outputs")) {
                                validate_service_output_base(
                                    output, allowed_output_kinds);
                                const std::string kind =
                                    output.at("output_kind")
                                        .get<std::string>();
                                const std::string output_id =
                                    output.at("output_id")
                                        .get<std::string>();
                                if (kind == "evidence.dataset") {
                                    if (!output_id.starts_with("dataset:") ||
                                        !uuid_v4(
                                            std::string_view(output_id)
                                                .substr(8)) ||
                                        (output.at("state").value(
                                             "lifecycle", "") !=
                                             "collecting" &&
                                         output.at("state").value(
                                             "lifecycle", "") !=
                                             "completed") ||
                                        !output.contains("summary") ||
                                        !output.at("summary").is_object() ||
                                        !output.at("summary").contains(
                                            "observation_count") ||
                                        !output.at("summary").at(
                                            "observation_count")
                                            .is_number_unsigned() ||
                                        output.at("compatibility")
                                                .value(
                                                    "dataset_schema",
                                                    "") !=
                                            "uci_arena_evidence_dataset_v1" ||
                                        !output.at("compatibility")
                                             .contains(
                                                 "target_identity") ||
                                        !output.at("compatibility")
                                             .at("target_identity")
                                             .is_object()) {
                                        throw std::runtime_error(
                                            "Evidence dataset output is incompatible");
                                    }
                                    dataset_projection.push_back(
                                        {
                                            {"schema",
                                             "uci_arena_evidence_dataset_v1"},
                                            {"id", output_id.substr(8)},
                                            {"campaign",
                                             output.at("compatibility")
                                                 .value(
                                                     "campaign", "")},
                                            {"state",
                                             output.at("state").value(
                                                 "lifecycle", "")},
                                            {"target_identity",
                                             bounded_redacted_external_value(
                                                 output.at(
                                                     "compatibility")
                                                     .at(
                                                         "target_identity"),
                                                 64 * 1024,
                                                 "Evidence target identity")},
                                            {"authority",
                                             bounded_redacted_external_value(
                                                 output.at("authority"),
                                                 64 * 1024,
                                                 "Evidence authority")},
                                            {"observation_count",
                                             output.at("summary").at(
                                                 "observation_count")},
                                            {"created_at",
                                             output.at("created_at")},
                                            {"completed_at",
                                             output.at("state").value(
                                                 "completed_at",
                                                 json(nullptr))},
                                        });
                                } else {
                                    if (!output_id.starts_with("profile:") ||
                                        !lowercase_hex(
                                            std::string_view(output_id)
                                                .substr(8),
                                            64) ||
                                        !output.contains("artifact") ||
                                        !output.at("artifact").is_object() ||
                                        !output.at("artifact").contains(
                                            "path") ||
                                        !output.at("artifact").at("path")
                                             .is_string() ||
                                        !lowercase_hex(
                                            output.at("artifact")
                                                .value("sha256", ""),
                                            64) ||
                                        output.at("artifact").value(
                                            "sha256", "") !=
                                            output_id.substr(8) ||
                                        !output.at("compatibility")
                                             .contains(
                                                 "target_identity") ||
                                        !output.at("compatibility")
                                             .at("target_identity")
                                             .is_object() ||
                                        !output.at("authority").value(
                                            "timing", false)) {
                                        throw std::runtime_error(
                                            "Evidence timing-profile output is incompatible");
                                    }
                                    json projected{
                                        {"schema",
                                         "uci_arena_evidence_profile_v1"},
                                        {"source",
                                         output.at("state").value(
                                             "source", "")},
                                        {"path",
                                         output.at("artifact").at(
                                             "path")},
                                        {"sha256",
                                         output.at("artifact").at(
                                             "sha256")},
                                        {"profile_schema",
                                         output.at("compatibility")
                                             .value(
                                                 "profile_schema",
                                                 "")},
                                        {"qualified", true},
                                        {"published_at",
                                         output.at("created_at")},
                                        {"identity_sha256",
                                         output.at("compatibility")
                                             .value(
                                                 "identity_sha256",
                                                 "")},
                                        {"target_identity",
                                         bounded_redacted_external_value(
                                             output.at(
                                                 "compatibility")
                                                 .at(
                                                     "target_identity"),
                                             64 * 1024,
                                             "Evidence timing-profile target identity")},
                                        {"permissions",
                                         bounded_redacted_external_value(
                                             output.at("authority"),
                                             64 * 1024,
                                             "Evidence timing-profile authority")},
                                        {"service_publication",
                                         bounded_redacted_external_value(
                                             output.value(
                                                 "provenance",
                                                 json::object())
                                                 .value(
                                                     "service_publication",
                                                     json(nullptr)),
                                             64 * 1024,
                                             "Evidence timing-profile publication")},
                                    };
                                    profile_projection.push_back(projected);
                                    if (selected_profile.is_null() ||
                                        (projected.value(
                                             "source", "") ==
                                             "service_live_cache" &&
                                         selected_profile.value(
                                             "source", "") !=
                                             "service_live_cache") ||
                                        (projected.value(
                                             "source", "") ==
                                             selected_profile.value(
                                                 "source", "") &&
                                         projected.value(
                                             "published_at", "") >
                                             selected_profile.value(
                                                 "published_at", ""))) {
                                        selected_profile =
                                            std::move(projected);
                                    }
                                }
                            }
                            data["output_integration"] = {
                                {"authority",
                                 "uci_arena_evidence_service"},
                                {"dataset_count",
                                 dataset_projection.size()},
                                {"datasets",
                                 std::move(dataset_projection)},
                                {"timing_profile_count",
                                 profile_projection.size()},
                                {"timing_profiles",
                                 std::move(profile_projection)},
                                {"selected_timing_profile",
                                 std::move(selected_profile)},
                            };
                        } else {
                            json snapshots = json::array();
                            for (const auto& output :
                                 listed.at("outputs")) {
                                validate_service_output_base(
                                    output, allowed_output_kinds);
                                const json& state =
                                    output.at("state");
                                const json& authority =
                                    output.at("authority");
                                const json& artifact =
                                    output.at("artifact");
                                const json& compatibility =
                                    output.at("compatibility");
                                const std::string channel =
                                    state.value("channel", "");
                                const std::string snapshot_id =
                                    output.at("output_id")
                                        .get<std::string>();
                                if ((channel != "integration" &&
                                     channel != "qualified") ||
                                    !snapshot_id.starts_with(
                                        channel + "-") ||
                                    state.value("immutable", false) !=
                                        true ||
                                    compatibility.value(
                                        "component_id", "") !=
                                        "opening_book.standard" ||
                                    compatibility.value(
                                        "consumer_component_id", "") !=
                                        "uci_arena.engine" ||
                                    !output.contains("summary") ||
                                    !output.at("summary").is_object() ||
                                    !output.at("summary").contains(
                                        "record_count") ||
                                    (!output.at("summary").at(
                                          "record_count")
                                          .is_number_unsigned() &&
                                     !output.at("summary").at(
                                          "record_count")
                                          .is_number_integer()) ||
                                    output.at("summary").at(
                                        "record_count")
                                            .get<std::int64_t>() < 0 ||
                                    !artifact.is_object() ||
                                    !artifact.contains("root") ||
                                    !artifact.at("root").is_string() ||
                                    !artifact.contains(
                                        "manifest_path") ||
                                    !artifact.at("manifest_path")
                                         .is_string() ||
                                    !lowercase_hex(
                                        artifact.value(
                                            "manifest_sha256", ""),
                                        64) ||
                                    !artifact.contains("files") ||
                                    !artifact.at("files").is_array() ||
                                    artifact.at("files").size() > 64) {
                                    throw std::runtime_error(
                                        "Book Forge snapshot output is incompatible");
                                }
                                for (const auto& file :
                                     artifact.at("files")) {
                                    if (!file.is_object() ||
                                        !file.contains("name") ||
                                        !file.at("name").is_string() ||
                                        file.at("name")
                                                .get_ref<
                                                    const std::string&>()
                                                .size() > 256 ||
                                        !lowercase_hex(
                                            file.value("sha256", ""),
                                            64)) {
                                        throw std::runtime_error(
                                            "Book Forge artifact digest list is malformed");
                                    }
                                }
                                const std::filesystem::path artifact_root =
                                    std::filesystem::path(
                                        artifact.at("root")
                                            .get<std::string>())
                                        .lexically_normal();
                                const std::filesystem::path manifest_path =
                                    std::filesystem::path(
                                        artifact.at("manifest_path")
                                            .get<std::string>())
                                        .lexically_normal();
                                const std::filesystem::path relative_manifest =
                                    manifest_path.lexically_relative(
                                        artifact_root);
                                if (!artifact_root.is_absolute() ||
                                    !manifest_path.is_absolute() ||
                                    relative_manifest.empty() ||
                                    relative_manifest.is_absolute() ||
                                    *relative_manifest.begin() == "..") {
                                    throw std::runtime_error(
                                        "Book Forge artifact path escapes its declared root");
                                }
                                snapshots.push_back(
                                    {
                                        {"snapshot_id",
                                         snapshot_id},
                                        {"channel", channel},
                                        {"manifest_sha256",
                                         artifact.at(
                                             "manifest_sha256")},
                                        {"artifact_files",
                                         artifact.at("files")},
                                        {"record_count",
                                         output.value(
                                             "summary",
                                             json::object())
                                             .value(
                                                 "record_count", 0)},
                                        {"qualified",
                                         authority.value(
                                             "strength_qualified",
                                             false)},
                                        {"includes_fixture",
                                         authority.value(
                                             "includes_fixture",
                                             false)},
                                        {"current",
                                         state.value(
                                             "current", false)},
                                        {"published_at",
                                         output.at("created_at")},
                                    });
                            }
                            data["output_integration"] = {
                                {"authority", "book_forge"},
                                {"snapshots", std::move(snapshots)},
                            };
                        }
                        data["output_cursor"] =
                            listed.at("cursor");
                    }

                    const json control =
                        status_payload.value(
                            "control", json::object());
                    const json daemon =
                        status_payload.value(
                            "daemon", json::object());
                    const bool paused =
                        control.value("paused", false);
                    const std::string daemon_phase =
                        daemon.value("phase", "stopped");
                    const std::string last_error =
                        daemon.value(
                            "lastError",
                            daemon.value("last_error", ""));
                    const bool daemon_active =
                        process_supervisor_.running(id) ||
                        (!daemon.value("stale", false) &&
                         daemon_phase != "stopped" &&
                         daemon_phase != "failed" &&
                         !daemon_phase.empty());
                    const bool degraded =
                        daemon_phase == "failed" ||
                        !last_error.empty();
                    data["integration_state"] = "available";
                    data["observed_state"] =
                        degraded ? "degraded" :
                        paused ? "paused" :
                        daemon_active ? "running" : "stopped";
                    data["health"] = {
                        {"state",
                         degraded ? "degraded" : "healthy"},
                        {"reason",
                         degraded
                             ? (last_error.empty()
                                    ? "service daemon failed"
                                    : last_error)
                             : "Manager service API contracts validated"},
                        {"observed_at", domain::utc_now()},
                    };
                    data["manager_api_projection"] = {
                        {"schema",
                         declaration.at("schema")},
                        {"api_version",
                         describe_response.at("api_version")},
                        {"component_id",
                         expected_component_id},
                        {"component_version",
                         expected_component_version},
                        {"source",
                         data.value(
                             "manager_integration_api_source",
                             "unknown")},
                        {"operations",
                         describe.at("operations")},
                        {"output_kinds",
                         describe.at("output_kinds")},
                    };
                    data["status_projection"] =
                        bounded_redacted_external_value(
                            status_payload,
                            1024 * 1024,
                            "External service status");
                } catch (const std::exception& error) {
                    data["integration_state"] = "incompatible";
                    data["observed_state"] = "degraded";
                    data["health"] = {
                        {"state", "degraded"},
                        {"reason", error.what()},
                        {"observed_at", domain::utc_now()},
                    };
                    data.erase("output_integration");
                    data.erase("output_cursor");
                    data.erase("manager_api_projection");
                }
            } else {
            const json status = process_supervisor_.run_json(
                node, cli.parent_path(), arguments);
            json status_payload =
                status.contains("status") && status.at("status").is_object()
                    ? status.at("status")
                    : status;
            const std::string expected_schema = evidence_service
                ? "uci_arena_evidence_status_v1"
                : "uci_arena_book_forge_status_v2";
            const std::string actual_schema =
                status_payload.value("schema", "");
            constexpr std::string_view book_forge_status_suffix =
                "_book_forge_status_v2";
            const bool compatible_schema =
                actual_schema == expected_schema ||
                (!evidence_service &&
                 actual_schema.size() > book_forge_status_suffix.size() &&
                 actual_schema.ends_with(book_forge_status_suffix));
            if (!compatible_schema) {
                data["integration_state"] = "incompatible";
                data["observed_state"] = "degraded";
                data["health"] = {
                    {"state", "degraded"},
                    {"reason",
                     evidence_service
                         ? "UCI Arena Evidence Service status schema is incompatible"
                         : "Book Forge status schema is incompatible"},
                };
            } else {
                status_payload["schema"] = expected_schema;
                try {
                    if (evidence_service) {
                        auto dataset_arguments = base;
                        dataset_arguments.insert(
                            dataset_arguments.end(), {"dataset", "list"});
                        add_locator_options(dataset_arguments);
                        dataset_arguments.push_back("--json");
                        const json datasets = process_supervisor_.run_json(
                            node, cli.parent_path(), dataset_arguments);
                        if (!datasets.is_array() || datasets.size() > 10'000 ||
                            status_payload.value("datasets", datasets.size()) !=
                                datasets.size()) {
                            throw std::runtime_error(
                                "Evidence Service dataset projection is inconsistent");
                        }
                        json dataset_projection = json::array();
                        for (const auto& dataset : datasets) {
                            if (dataset.value("schema", "") !=
                                    "uci_arena_evidence_dataset_v1" ||
                                !dataset.contains("id") ||
                                !dataset.at("id").is_string() ||
                                dataset.at("id").get_ref<
                                    const std::string&>().size() > 128 ||
                                !dataset.contains("targetIdentity") ||
                                !dataset.at("targetIdentity").is_object() ||
                                !dataset.contains("authority") ||
                                !dataset.at("authority").is_object() ||
                                !dataset.contains("observationCount") ||
                                !dataset.at("observationCount").is_number_unsigned() ||
                                (dataset.value("state", "") != "collecting" &&
                                 dataset.value("state", "") != "completed")) {
                                throw std::runtime_error(
                                    "Evidence Service dataset identity contract mismatch");
                            }
                            dataset_projection.push_back(
                                {
                                    {"schema", "uci_arena_evidence_dataset_v1"},
                                    {"id", dataset.at("id")},
                                    {"campaign",
                                     dataset.value("campaign", "")},
                                    {"state", dataset.at("state")},
                                    {"target_identity",
                                     bounded_redacted_external_value(
                                         dataset.at("targetIdentity"),
                                         64 * 1024,
                                         "Evidence target identity")},
                                    {"authority",
                                     bounded_redacted_external_value(
                                         dataset.at("authority"),
                                         64 * 1024,
                                         "Evidence authority")},
                                    {"observation_count",
                                     dataset.at("observationCount")},
                                    {"created_at",
                                     dataset.contains("createdAt") &&
                                             dataset.at("createdAt").is_string()
                                         ? dataset.at("createdAt")
                                         : json("")},
                                    {"completed_at",
                                     dataset.contains("completedAt") &&
                                             dataset.at("completedAt").is_string()
                                         ? dataset.at("completedAt")
                                         : json(nullptr)},
                                });
                        }

                        auto profile_arguments = base;
                        profile_arguments.insert(
                            profile_arguments.end(), {"profile", "list"});
                        add_locator_options(profile_arguments);
                        profile_arguments.push_back("--json");
                        const json profiles = process_supervisor_.run_json(
                            node, cli.parent_path(), profile_arguments);
                        if (!profiles.is_array() || profiles.size() > 64) {
                            throw std::runtime_error(
                                "Evidence Service timing-profile projection is invalid");
                        }
                        json profile_projection = json::array();
                        json selected_profile = nullptr;
                        for (const auto& profile : profiles) {
                            const std::string source =
                                required_string(profile, "source");
                            const std::string path =
                                required_string(profile, "path");
                            const std::string sha256 =
                                required_string(profile, "sha256");
                            const std::string identity_sha256 =
                                required_string(
                                    profile, "identitySha256");
                            if (profile.value("schema", "") !=
                                    "uci_arena_evidence_profile_v1" ||
                                (source != "service_live_cache" &&
                                 source != "explicit_import") ||
                                path.size() > 32 * 1024 ||
                                sha256.size() != 64 ||
                                !std::ranges::all_of(
                                    sha256,
                                    [](const unsigned char character) {
                                        return std::isxdigit(character) != 0;
                                    }) ||
                                identity_sha256.size() != 71 ||
                                !identity_sha256.starts_with("sha256:") ||
                                profile.value("qualified", false) != true ||
                                !profile.contains("targetIdentity") ||
                                !profile.at("targetIdentity").is_object() ||
                                !profile.contains("permissions") ||
                                !profile.at("permissions").is_object()) {
                                throw std::runtime_error(
                                    "Evidence Service timing-profile identity contract mismatch");
                            }
                            const json permissions =
                                profile.at("permissions");
                            for (const auto* field :
                                 {"provider_safety_qualified",
                                  "forecast_shadow_qualified",
                                  "adaptive_play_qualified"}) {
                                if (!permissions.contains(field) ||
                                    !permissions.at(field).is_boolean()) {
                                    throw std::runtime_error(
                                        "Evidence Service timing-profile permissions are incomplete");
                                }
                            }
                            json projected{
                                {"schema",
                                 "uci_arena_evidence_profile_v1"},
                                {"source", source},
                                {"path", path},
                                {"sha256", sha256},
                                {"profile_schema",
                                 profile.value("profileSchema", "")},
                                {"qualified", true},
                                {"published_at",
                                 profile.value("publishedAt", "")},
                                {"identity_sha256", identity_sha256},
                                {"target_identity",
                                 bounded_redacted_external_value(
                                     profile.at("targetIdentity"),
                                     64 * 1024,
                                     "Evidence timing-profile target identity")},
                                {"permissions", permissions},
                                {"service_publication",
                                 profile.contains("servicePublication")
                                     ? bounded_redacted_external_value(
                                           profile.at("servicePublication"),
                                           64 * 1024,
                                           "Evidence timing-profile publication")
                                     : json(nullptr)},
                            };
                            profile_projection.push_back(projected);
                            if (selected_profile.is_null() ||
                                (source == "service_live_cache" &&
                                 selected_profile.value("source", "") !=
                                     "service_live_cache") ||
                                (source ==
                                     selected_profile.value("source", "") &&
                                 projected.value("published_at", "") >
                                     selected_profile.value(
                                         "published_at", ""))) {
                                selected_profile = std::move(projected);
                            }
                        }

                        const json control =
                            status_payload.value("control", json::object());
                        const json collection =
                            status_payload.value("collection", json::object());
                        const json daemon =
                            status_payload.value("daemon", json::object());
                        const bool paused = control.value("paused", false);
                        const std::string daemon_phase =
                            daemon.value("phase", "stopped");
                        const std::string last_error =
                            daemon.value("lastError", "");
                        const bool daemon_active =
                            process_supervisor_.running(id) ||
                            (!daemon.value("stale", false) &&
                             daemon_phase != "stopped" &&
                             daemon_phase != "failed" &&
                             !daemon_phase.empty());
                        const bool degraded =
                            daemon_phase == "failed" || !last_error.empty();
                        data["integration_state"] = "available";
                        data["observed_state"] =
                            degraded ? "degraded" :
                            paused ? "paused" :
                            daemon_active ? "running" : "stopped";
                        data["health"] = {
                            {"state", degraded ? "degraded" : "healthy"},
                            {"reason",
                             degraded ?
                                 last_error.empty() ?
                                     "Evidence Service daemon failed" :
                                     last_error :
                                 collection.value(
                                     "reason",
                                     "Evidence Service status and dataset contracts validated")},
                            {"observed_at", domain::utc_now()},
                        };
                        data["output_integration"] = {
                            {"authority", "uci_arena_evidence_service"},
                            {"dataset_count", datasets.size()},
                            {"datasets", std::move(dataset_projection)},
                            {"timing_profile_count", profiles.size()},
                            {"timing_profiles",
                             std::move(profile_projection)},
                            {"selected_timing_profile",
                             std::move(selected_profile)},
                        };
                    } else {
                        json snapshots = json::array();
                        const json channels =
                            status_payload.value("channels", json::array());
                        if (!channels.is_array() || channels.size() > 2) {
                            throw std::runtime_error(
                                "Book Forge channel projection is invalid");
                        }
                        for (const auto& channel : channels) {
                            const std::string channel_name =
                                required_string(channel, "channel");
                            const std::string snapshot_id =
                                required_string(channel, "snapshot_id");
                            const std::string directory =
                                required_string(channel, "directory");
                            if (channel_name != "integration" &&
                                channel_name != "qualified") {
                                throw std::runtime_error(
                                    "Book Forge advertised an unknown channel");
                            }
                            auto validate_arguments = base;
                            validate_arguments.insert(
                                validate_arguments.end(),
                                {"validate", directory});
                            add_locator_options(validate_arguments);
                            const json validation =
                                process_supervisor_.run_json(
                                    node,
                                    cli.parent_path(),
                                    validate_arguments);
                            if (!validation.value("ok", false) ||
                                validation.value("id", "") != snapshot_id ||
                                validation.value("channel", "") != channel_name ||
                                !validation.contains("manifestSha256") ||
                                !validation.at("manifestSha256").is_string()) {
                                throw std::runtime_error(
                                    "Book Forge snapshot validation mismatch");
                            }
                            snapshots.push_back(
                                {{"snapshot_id", snapshot_id},
                                 {"channel", channel_name},
                                 {"manifest_sha256",
                                  validation.at("manifestSha256")},
                                 {"record_count",
                                  validation.value("recordCount", 0)},
                                 {"qualified",
                                  validation.value("qualified", false)},
                                 {"includes_fixture",
                                  validation.value("includesFixture", false)},
                                 {"published_at",
                                  channel.value("published_at", "")}});
                        }
                        data["integration_state"] = "available";
                        data["observed_state"] =
                            status_payload.value("control", json::object())
                                    .value("paused", false)
                                ? "paused"
                                : process_supervisor_.running(id) ||
                                          (!status_payload
                                                .value("daemon", json::object())
                                                .value("stale", false) &&
                                           status_payload
                                                   .value("daemon", json::object())
                                                   .value("phase", "stopped") !=
                                               "stopped" &&
                                           status_payload
                                                   .value("daemon", json::object())
                                                   .value("phase", "failed") !=
                                               "failed")
                                      ? "running"
                                      : "stopped";
                        data["health"] = {
                            {"state", "healthy"},
                            {"reason",
                             "Book Forge status and snapshot contracts validated"},
                            {"observed_at", domain::utc_now()},
                        };
                        data["output_integration"] = {
                            {"authority", "book_forge"},
                            {"snapshots", std::move(snapshots)},
                        };
                    }
                    data["status_projection"] =
                        bounded_redacted_external_value(
                            status_payload,
                            1024 * 1024,
                            "External service status");
                } catch (const std::exception& error) {
                    data["integration_state"] = "incompatible";
                    data["observed_state"] = "degraded";
                    data["health"] = {
                        {"state", "degraded"},
                        {"reason", error.what()},
                        {"observed_at", domain::utc_now()},
                    };
                    data.erase("output_integration");
                }
            }
            }
        }
    } else {
        throw CommandFailure("unknown_action", "unsupported service action: " + action, 400);
    }

    ports::ResourceRecord updated = *existing;
    updated.revision = existing->revision + 1;
    updated.updated_at = domain::utc_now();
    updated.data_json = data.dump();
    return complete_update(
        request,
        actor,
        std::move(updated),
        existing->revision,
        "managed_service." + action.substr(std::string("service.").size()));
}

json ControlPlane::remove_resource(const json& request, const std::string& actor) {
    const auto& target = request.at("target");
    const std::string kind = required_string(target, "kind");
    const std::string id = required_string(target, "id");
    const auto existing = store_->get_resource(kind, id);
    if (!existing) {
        throw CommandFailure("resource_not_found", "resource does not exist", 404);
    }
    const std::uint64_t expected_revision =
        request.contains("expected_revision") && !request.at("expected_revision").is_null()
            ? request.at("expected_revision").get<std::uint64_t>()
            : existing->revision;
    json result = {
        {"schema_version", 1},
        {"request_id", request.at("request_id")},
        {"command_id", domain::uuid_v7()},
        {"status", "completed"},
        {"dry_run", request.at("dry_run")},
        {"resource", resource_json(*existing)},
        {"operation_id", nullptr},
        {"error", nullptr},
    };
    if (request.at("dry_run").get<bool>()) {
        return result;
    }
    const std::string now = domain::utc_now();
    ports::CommandRecord command{
        result.at("command_id"),
        request.at("request_id"),
        request.at("idempotency_key"),
        request.at("action"),
        "completed",
        result.dump(),
        now,
    };
    ports::EventRecord event{
        0,
        domain::uuid_v7(),
        now,
        "state",
        kind + ".removed",
        kind,
        id,
        expected_revision,
        result.at("command_id").get<std::string>(),
        json{{"actor", actor}, {"action", request.at("action")}}.dump(),
    };
    store_->delete_resource(kind, id, expected_revision, command, event);
    return result;
}

json ControlPlane::resource_json(const ports::ResourceRecord& record) {
    return {
        {"id", record.id},
        {"host_id", record.host_id},
        {"kind", record.kind},
        {"revision", record.revision},
        {"created_at", record.created_at},
        {"updated_at", record.updated_at},
        {"labels", json::parse(record.labels_json)},
        {"data", json::parse(record.data_json)},
    };
}

json ControlPlane::event_json(const ports::EventRecord& record) {
    json resource = nullptr;
    if (record.resource_kind && record.resource_id && record.resource_revision) {
        resource = {
            {"kind", *record.resource_kind},
            {"id", *record.resource_id},
            {"host_id", "local"},
            {"revision", *record.resource_revision},
        };
    }
    return {
        {"schema_version", 1},
        {"cursor", record.cursor},
        {"event_id", record.event_id},
        {"timestamp", record.timestamp},
        {"stream", record.stream},
        {"kind", record.kind},
        {"resource", resource},
        {"correlation_id", record.correlation_id ? json(*record.correlation_id) : json(nullptr)},
        {"payload", json::parse(record.payload_json)},
    };
}

json ControlPlane::list(const std::string& kind, std::uint32_t limit, std::uint32_t offset) const {
    json result = json::array();
    for (const auto& record : store_->list_resources(kind, std::min(limit, 1000U), offset)) {
        result.push_back(resource_json(record));
    }
    return result;
}

json ControlPlane::events_after(std::uint64_t cursor, std::uint32_t limit) const {
    json result = json::array();
    for (const auto& record : store_->events_after(cursor, std::min(limit, 1000U))) {
        result.push_back(event_json(record));
    }
    return result;
}

json problem_json(const CommandFailure& failure) {
    return {
        {"type", "https://uci-arena-manager.local/problems/" + failure.code()},
        {"title", "Command rejected"},
        {"status", failure.http_status()},
        {"code", failure.code()},
        {"detail", failure.what()},
    };
}

}  // namespace arena::application
