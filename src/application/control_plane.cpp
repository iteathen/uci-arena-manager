#include "arena/application/control_plane.hpp"

#include "arena/adapters/uci_process.hpp"
#include "arena/application/service_catalog.hpp"
#include "arena/domain/match_planner.hpp"
#include "arena/domain/uuid.hpp"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <set>
#include <stdexcept>
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

}  // namespace

CommandFailure::CommandFailure(std::string code, std::string detail, int http_status)
    : std::runtime_error(std::move(detail)), code_(std::move(code)), http_status_(http_status) {}

const std::string& CommandFailure::code() const noexcept { return code_; }
int CommandFailure::http_status() const noexcept { return http_status_; }

ControlPlane::ControlPlane(std::shared_ptr<ports::Store> store)
    : store_(std::move(store)), started_at_(domain::utc_now()) {}

void ControlPlane::initialize() { store_->initialize(); }

json ControlPlane::status() const {
    return {
        {"schema_version", 1},
        {"service", "uci-arena-manager"},
        {"version", "0.1.0"},
        {"host_id", "local"},
        {"started_at", started_at_},
        {"database", "ready"},
        {"counts", store_->resource_counts()},
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
        if (action == "bot.register" || action == "service.register" ||
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
    const std::string display_name = required_string(parameters, "display_name");
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
        action == "bot.register"
            ? "bot"
            : action == "service.register" ? "managed_service" : "health_probe";
    const json& parameters = request.at("parameters");
    json data;
    if (kind == "managed_service") {
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
        return ServiceCatalog::discover(profile_id);
    } catch (const std::exception& error) {
        throw CommandFailure("service_profile_not_found", error.what(), 404);
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
        json parameters = request.at("parameters");
        parameters["profile_id"] = profile;
        parameters["display_name"] =
            parameters.value("display_name", data.value("display_name", profile));
        try {
            data = ServiceCatalog::normalize_registration(parameters);
        } catch (const std::exception& error) {
            throw CommandFailure("invalid_service_profile", error.what(), 422);
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
    if (profile == "uci_arena_evidence") {
        const bool endpoint_configured =
            data.contains("endpoint") && data.at("endpoint").is_string() &&
            !data.at("endpoint").get<std::string>().empty();
        throw CommandFailure(
            "capability_unavailable",
            endpoint_configured
                ? "UCI Arena Evidence Service endpoint is configured; its versioned capability handshake is not available yet"
                : "configure an optional UCI Arena Evidence Service endpoint before using runtime actions",
            409);
    }
    if (profile != "book_forge") {
        throw CommandFailure("unsupported_service_profile", "service profile has no runtime adapter", 409);
    }

    const std::filesystem::path node = data.at("node_executable").get<std::string>();
    const std::filesystem::path cli = data.at("cli_entrypoint").get<std::string>();
    const std::filesystem::path workspace = data.at("workspace").get<std::string>();
    std::vector<std::string> base{cli.string()};
    if (action == "service.start") {
        auto arguments = base;
        arguments.insert(
            arguments.end(), {"daemon", "--workspace", workspace.string()});
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
            arguments.insert(
                arguments.end(), {"daemon", "--workspace", workspace.string()});
            process_supervisor_.start(id, node, cli.parent_path(), arguments);
        }
        data["desired_state"] = "running";
        data["observed_state"] = "running";
        data["integration_state"] = "available";
    } else if (action == "service.pause" || action == "service.resume") {
        auto arguments = base;
        arguments.insert(
            arguments.end(),
            {action == "service.pause" ? "pause" : "resume",
             "--workspace",
             workspace.string()});
        if (!request.at("dry_run").get<bool>()) {
            data["last_control_result"] = process_supervisor_.run_json(
                node, cli.parent_path(), arguments);
        }
        data["desired_state"] = action == "service.pause" ? "paused" : "running";
        data["observed_state"] = action == "service.pause" ? "paused" :
            (process_supervisor_.running(id) ? "running" : "stopped");
    } else if (action == "service.status") {
        auto arguments = base;
        arguments.insert(
            arguments.end(), {"status", "--workspace", workspace.string(), "--json"});
        if (!request.at("dry_run").get<bool>()) {
            const json status = process_supervisor_.run_json(
                node, cli.parent_path(), arguments);
            const json& status_payload =
                status.contains("status") && status.at("status").is_object()
                    ? status.at("status")
                    : status;
            if (status_payload.value("schema", "") != "flashfork_book_forge_status_v2") {
                data["integration_state"] = "incompatible";
                data["observed_state"] = "degraded";
                data["health"] = {
                    {"state", "degraded"},
                    {"reason", "Book Forge status schema is incompatible"},
                };
            } else {
                data["integration_state"] = "available";
                data["observed_state"] =
                    status_payload.value("control", json::object()).value("paused", false)
                        ? "paused"
                        : process_supervisor_.running(id) ? "running" : "stopped";
                data["health"] = {
                    {"state", "healthy"},
                    {"reason", "Book Forge status contract validated"},
                    {"observed_at", domain::utc_now()},
                };
                data["status_projection"] = redact_external_status(status_payload);
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
