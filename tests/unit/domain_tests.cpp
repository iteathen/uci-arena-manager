#include "arena/adapters/sqlite_store.hpp"
#include "arena/application/broker_policy.hpp"
#include "arena/application/control_plane.hpp"
#include "arena/application/manager_lifecycle.hpp"
#include "arena/application/service_catalog.hpp"
#include "arena/domain/uuid.hpp"
#include <sodium.h>
#ifdef _WIN32
#include "arena/platform/windows_install_scope.hpp"
#endif

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <map>
#include <vector>

namespace {

namespace fs = std::filesystem;
using nlohmann::json;

void write_json(const fs::path& path, const json& value) {
    fs::create_directories(path.parent_path());
    std::ofstream(path) << value.dump(2) << '\n';
}

json read_json(const fs::path& path) {
    std::ifstream input(path);
    json value;
    input >> value;
    return value;
}

std::string sha256_text(const std::string& value) {
    std::array<unsigned char, crypto_hash_sha256_BYTES> digest{};
    crypto_hash_sha256(
        digest.data(),
        reinterpret_cast<const unsigned char*>(value.data()),
        static_cast<unsigned long long>(value.size()));
    std::array<char, crypto_hash_sha256_BYTES * 2 + 1> encoded{};
    sodium_bin2hex(
        encoded.data(), encoded.size(), digest.data(), digest.size());
    return encoded.data();
}

json manager_service_api() {
    return {
        {"schema", "uci_arena_manager_service_api_v1"},
        {"version", "1.0"},
        {"transport", "cli_json"},
        {"entrypoint", "cli"},
        {"arguments", {"api"}},
        {"operations",
         {"describe",
           "status",
           "outputs_list",
           "outputs_inspect",
           "config_validate",
           "acquisition_retry_auth",
           "startup_recover"}},
        {"output_schema", "uci_arena_service_output_v1"},
    };
}

json manager_bot_options_fixture() {
    return {
        {"schema", "uci_arena_lichess_manager_options_v1"},
        {"capacity",
         {{"total_slots", 2},
          {"startup_parallelism", 2},
          {"human",
           {{"slots", 1},
            {"allow_borrowing", false},
            {"accept_public", true}}},
          {"bot", {{"slots", 1}, {"target_active", 1}}},
          {"evidence",
           {{"status", "qualified"},
            {"qualified", true},
            {"maximum_concurrency", 2}}}}},
        {"inbound_challenges",
         {{"accept_bots", true},
          {"min_base", 60},
          {"max_base", 1800},
          {"min_increment", 0},
          {"max_increment", 10},
          {"speeds", {"rapid"}},
          {"modes", {"casual"}}}},
        {"outbound_challenge",
         {{"rated", false},
          {"color", "random"},
          {"clock_limit", 600},
          {"clock_increment", 2}}},
        {"scheduler",
         {{"enabled", true},
          {"discovery_interval_seconds", 60},
          {"challenge_interval_seconds", 5},
          {"reservation_retry_seconds", 30},
          {"challenge_timeout_seconds", 25},
          {"first_move_timeout_seconds", 60}}},
        {"opponent_selection",
         {{"max_games_per_opponent_per_day", 1},
          {"timeout_cooldown_minutes", 30},
          {"decline_cooldown_minutes", 360},
          {"rating_target", 2200},
          {"rating_spread", 600}}},
    };
}

json provider_status_v3_fixture() {
    return {
        {"schema", "uci_arena_lichess_status_v3"},
        {"installed", true},
        {"registered", true},
        {"active", true},
        {"ready", false},
        {"stream_connected", false},
        {"stream_state", "waiting_for_api_cooldown"},
        {"runtime_updated_at", "2026-08-23T18:45:00.000Z"},
        {"account", {{"username", "ArenaBot"}, {"bot", true}}},
        {"capacity",
         {{"configured", 2},
          {"qualified", 2},
          {"slots",
           {{"empty", 1},
            {"preparing", 0},
            {"pending", 0},
            {"cancelling", 0},
            {"active", 1},
            {"draining", 0}}}}},
        {"games",
         {{"active", 1},
          {"preparing", 0},
          {"pending", 0},
          {"cancelling", 0},
          {"draining", 0}}},
        {"scheduler",
         {{"enabled", true},
          {"paused", false},
          {"target_active_bots", 1},
          {"fault", {{"kind", "api_request_failed"}, {"status", 429}}}}},
        {"rate_limit",
         {{"state", "cooldown"},
          {"cooldown_until", 1'777'000'000'000ULL},
          {"cooldown_remaining_ms", 45'000},
          {"strike_count", 2},
          {"challenge_state", "quarantined"},
          {"challenge_blocked_until", 1'777'000'000'000ULL},
          {"challenge_remaining_ms", 45'000},
          {"challenge_strike_count", 2},
          {"queue_depth", 1},
          {"in_flight_class", nullptr},
          {"client_lease",
           {{"state", "authoritative"},
            {"active_clients", 1},
            {"heartbeat_at", 1'776'999'950'000ULL},
            {"expires_at", 1'777'000'010'000ULL}}}}},
        {"blockers", {"rate_limit_cooldown"}},
    };
}

json provider_status_v4_fixture() {
    json status = provider_status_v3_fixture();
    status["schema"] = "uci_arena_lichess_status_v4";
    status["supervision"] = {
        {"state", "restart_pending"},
        {"restart_pending", true},
        {"exit_receipt_state", "ready"},
        {"last_exit",
         {{"disposition", "unexpected_exit"},
          {"exited_at", "2026-08-23T18:44:30.000Z"},
          {"child_exit_code", 3'221'225'481ULL}}},
    };
    status["incidents"] = {
        {"available", true},
        {"active_count", 2},
        {"highest_severity", "critical"},
        {"latest_cursor", 8},
        {"truncated", false},
        {"truncated_rows", 0},
    };
    return status;
}

json provider_status_v5_fixture() {
    json status = provider_status_v4_fixture();
    status["schema"] = "uci_arena_lichess_status_v5";
    status["games"].update({
        {"archive_available", true},
        {"total", 646},
        {"complete", 636},
        {"incomplete", 10},
        {"storage_bytes", 4'194'304},
    });
    return status;
}

json provider_incident_page_fixture(
    const std::string& filter = "active",
    const std::uint64_t after_cursor = 0) {
    return {
        {"schema", "uci_arena_provider_incident_page_v1"},
        {"filter", filter},
        {"after_cursor", after_cursor},
        {"next_cursor", after_cursor + 8},
        {"has_more", false},
        {"incidents",
         json::array({
             {{"cursor", after_cursor + 3},
              {"code", "api_request_failed"},
              {"severity", "error"},
              {"source", "api"},
              {"operation", "online_bots"},
              {"first_occurrence", 1'777'000'000'000ULL},
              {"last_occurrence", 1'777'000'010'000ULL},
              {"occurrence_count", 2},
              {"state", "active"},
              {"resolved_at", nullptr},
              {"http_status", 429},
              {"retry_deadline", 1'777'000'060'000ULL},
              {"game_id", nullptr},
              {"detail", "Lichess request retry window 1/2 elapsed with HTTP 429"}},
             {{"cursor", after_cursor + 8},
              {"code", "stream_disconnected"},
              {"severity", "critical"},
              {"source", "stream"},
              {"operation", "event_stream"},
              {"first_occurrence", 1'777'000'020'000ULL},
              {"last_occurrence", 1'777'000'030'000ULL},
              {"occurrence_count", 1},
              {"state", "active"},
              {"resolved_at", nullptr},
              {"http_status", nullptr},
              {"retry_deadline", 1'777'000'060'000ULL},
              {"game_id", "fixture-game"},
              {"detail", "Provider event stream disconnected"}},
         })},
        {"retention",
         {{"maximum_rows", 2'048},
          {"resolved_retention_ms", 7'776'000'000ULL},
          {"last_truncated_cursor", nullptr},
          {"truncated_rows", 0},
          {"truncated_active_rows", 0}}},
    };
}

json provider_game_summary_fixture(
    const std::string& state,
    const std::uint64_t cursor,
    const std::string& game_id,
    const std::string& completeness = "complete") {
    const bool live = state == "live";
    const bool summary_only = completeness == "summary_only";
    const bool complete =
        completeness == "complete" || completeness == "reconciled";
    return {
        {"cursor", cursor},
        {"game_id", game_id},
        {"state", state},
        {"opponent", "FixtureBot"},
        {"color", "white"},
        {"rated", false},
        {"speed", "blitz"},
        {"variant", "standard"},
        {"clock_limit", 300},
        {"clock_increment", 3},
        {"status", live ? "started" : "mate"},
        {"result", live ? json(nullptr) : json("win")},
        {"winner", live ? json(nullptr) : json("white")},
        {"plies", complete ? 2 : 0},
        {"started_at", 1'777'000'000'000ULL + cursor},
        {"finished_at",
         live ? json(nullptr) : json(1'777'000'010'000ULL + cursor)},
        {"archive_completeness", completeness},
        {"incomplete_reason",
         summary_only ? json("historical_moves_unavailable") : json(nullptr)},
        {"archive_sha256",
         complete ? json("sha256:" + std::string(64, 'a')) : json(nullptr)},
        {"provider_component",
         {{"component_id", "lichess.adapter"},
          {"version", "1.0.24"},
          {"manifest_sha256", "sha256:" + std::string(64, 'b')}}},
        {"engine_component", nullptr},
    };
}

json provider_game_page_fixture(
    const std::string& scope = "history",
    const std::uint64_t before_cursor = 0) {
    return {
        {"schema", "uci_arena_provider_game_page_v1"},
        {"scope", scope},
        {"before_cursor", before_cursor},
        {"next_cursor", 7},
        {"has_more", false},
        {"games",
         json::array({
             provider_game_summary_fixture(
                 scope, 9, "fixture-game-nine",
                 scope == "live" ? "live" : "complete"),
             provider_game_summary_fixture(
                 scope, 7, "fixture-game-seven",
                 scope == "live" ? "live" : "summary_only"),
         })},
    };
}

json provider_game_detail_fixture(
    const std::string& game_id = "fixture-game-nine") {
    json game = provider_game_summary_fixture(
        "history", 9, game_id, "complete");
    game["schema"] = "uci_arena_provider_game_v1";
    game["initial_fen"] =
        "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1";
    game["runtime_provenance"] = {
        {"sha256", "sha256:" + std::string(64, 'c')},
        {"details_available", false},
    };
    game["replay"] = json::array({
        {{"ply", 0},
         {"uci_move", "e2e4"},
         {"before_fen", game.at("initial_fen")},
         {"after_fen",
          "rnbqkbnr/pppppppp/8/8/4P3/8/PPPP1PPP/RNBQKBNR b KQkq - 0 1"},
         {"actor_color", "white"},
         {"clock_ms", 300'000}},
        {{"ply", 1},
         {"uci_move", "e7e5"},
         {"before_fen",
          "rnbqkbnr/pppppppp/8/8/4P3/8/PPPP1PPP/RNBQKBNR b KQkq - 0 1"},
         {"after_fen",
          "rnbqkbnr/pppp1ppp/8/4p3/4P3/8/PPPP1PPP/RNBQKBNR w KQkq - 0 2"},
         {"actor_color", "black"},
         {"clock_ms", 299'000}},
    });
    return game;
}

void write_installed_component(
    const fs::path& data_root,
    const fs::path& install_root,
    const std::string& id,
    const std::string& entrypoint_name,
    const std::string& entrypoint_path,
    const std::vector<fs::path>& data_paths = {},
    const json& manifest_fields = json::object()) {
    const fs::path active = install_root / id / "1.0.0";
    const fs::path entrypoint = active / entrypoint_path;
    fs::create_directories(entrypoint.parent_path());
    std::ofstream(entrypoint, std::ios::binary) << "fixture";
    json manifest{
        {"schema", "arena_install_component_v1"},
        {"component_id", id},
        {"component_version", "1.0.0"},
        {"entrypoints", {{entrypoint_name, entrypoint_path}}},
        {"files",
         json::array(
             {{{"path", entrypoint_path},
               {"sha256",
                "f16d05ec6b29248d2c61adb1e9263f78e4f7bace1b955014a2d17872cfe4064d"},
               {"size_bytes", 7}}})},
    };
    manifest.update(manifest_fields);
    write_json(active / "arena-component.json", manifest);
    json paths = json::array();
    for (const auto& path : data_paths) {
        fs::create_directories(path);
        paths.push_back(path.string());
    }
    write_json(
        data_root / "receipts" / (id + ".json"),
        {
            {"schema", "arena_install_receipt_v1"},
            {"component_id", id},
            {"component_version", "1.0.0"},
            {"state", "installed_managed"},
            {"active_root", active.string()},
            {"artifact_sha256", std::string(64, 'a')},
            {"data_paths", std::move(paths)},
        });
}

void attach_bot_provider_contract(
    const fs::path& data_root,
    const bool direct_invocation = false,
    const bool alternate_operation_prefixes = false,
    const std::string& contract_version = "1.0",
    const bool installed_qualification = true,
    const bool provider_incidents = false,
    const bool provider_games = false) {
    const fs::path receipt_path =
        data_root / "receipts" / "lichess.adapter.json";
    json receipt = read_json(receipt_path);
    const fs::path active_root =
        receipt.at("active_root").get<std::string>();
    const fs::path manifest_path = active_root / "arena-component.json";
    json manifest = read_json(manifest_path);
    const std::string entrypoint =
        manifest.at("entrypoints").at("adapter").get<std::string>();
    manifest["entrypoints"]["cli"] = entrypoint;
    manifest["contract_manifest"] = "contracts/service-contract.json";
    manifest["dependencies"] = direct_invocation
        ? json::array()
        : json::array({"node_runtime.private"});

    json operations{
        {"schema", "fixture_provider_manager_api_v1"},
        {"version", contract_version},
        {"transport",
         {{"kind", "cli_json"},
          {"entrypoint", entrypoint},
          {"stdout", "one_bounded_json_document"},
          {"stderr", "sanitized_diagnostics"},
          {"shell", false}}},
        {"operations",
          {{"account_probe",
           {{"arguments",
             alternate_operation_prefixes
                 ? json::array({"identity", "check"})
                 : json::array({"probe"})},
            {"mutates", false},
            {"credential", "external_file_path_only"}}},
          {"config_get",
           {{"arguments",
             alternate_operation_prefixes
                 ? json::array({"settings", "show"})
                 : json::array({"config", "get"})},
            {"mutates", false},
            {"credential", "forbidden"}}},
          {"config_update",
           {{"arguments", json::array({"config", "update"})},
            {"mutates", true},
            {"credential", "forbidden"}}},
          {"owner_list",
           {{"arguments", json::array({"owner", "list"})},
            {"mutates", false}, {"credential", "forbidden"}}},
          {"owner_add",
           {{"arguments", json::array({"owner", "add"})},
            {"mutates", true}, {"credential", "forbidden"}}},
          {"owner_remove",
           {{"arguments", json::array({"owner", "remove"})},
            {"mutates", true}, {"credential", "forbidden"}}},
          {"provider_status",
           {{"arguments",
             alternate_operation_prefixes
                 ? json::array({"provider", "observe"})
                 : json::array({"status"})},
             {"mutates", false}, {"credential", "forbidden"}}},
          {"qualification_run",
           {{"arguments",
             alternate_operation_prefixes
                 ? json::array({"evidence", "refresh"})
                 : json::array({"qualification", "run"})},
            {"mutates", true}, {"credential", "forbidden"}}},
          {"reservation_cancel",
           {{"arguments", json::array({"reservation", "cancel"})},
            {"mutates", true}, {"credential", "forbidden"}}},
          {"reservation_create",
           {{"arguments", json::array({"reserve"})},
            {"mutates", true}, {"credential", "forbidden"}}},
          {"reservation_status",
           {{"arguments", json::array({"reservation", "status"})},
            {"mutates", false}, {"credential", "forbidden"}}},
          {"scheduler_pause",
           {{"arguments", json::array({"scheduler", "pause"})},
            {"mutates", true}, {"credential", "forbidden"}}},
          {"scheduler_resume",
           {{"arguments", json::array({"scheduler", "resume"})},
            {"mutates", true}, {"credential", "forbidden"}}},
          {"scheduler_status",
           {{"arguments", json::array({"scheduler", "status"})},
            {"mutates", false}, {"credential", "forbidden"}}},
          {"startup_install",
           {{"arguments", json::array({"startup", "install"})},
            {"mutates", true},
            {"credential", "external_file_path_only"}}},
          {"startup_status",
           {{"arguments", json::array({"startup", "status"})},
            {"mutates", false}, {"credential", "forbidden"}}},
          {"startup_recover",
           {{"arguments", json::array({"startup", "recover"})},
            {"mutates", true}, {"credential", "forbidden"}}},
          {"startup_uninstall",
           {{"arguments", json::array({"startup", "uninstall"})},
            {"mutates", true}, {"credential", "forbidden"}}}}},
    };
    if (!installed_qualification) {
        operations["operations"].erase("qualification_run");
    }
    if (provider_incidents) {
        operations["operations"]["incidents_list"] = {
            {"arguments",
             alternate_operation_prefixes
                 ? json::array({"alerts", "scan"})
                 : json::array({"incidents", "list"})},
            {"optional_arguments",
             json::array(
                 {"--filter", "--after-cursor", "--limit", "--runtime-dir"})},
            {"mutates", false},
            {"credential", "forbidden"},
            {"response", "uci_arena_provider_incident_page_v1"},
            {"response_schema", "contracts/incident-page-schema.json"},
        };
        manifest["contracts"]["incident_page"] =
            "contracts/incident-page-schema.json";
    }
    if (provider_games) {
        operations["operations"]["games_list"] = {
            {"arguments",
             alternate_operation_prefixes
                 ? json::array({"matches", "scan"})
                 : json::array({"games", "list"})},
            {"optional_arguments",
             json::array({"--scope", "--before", "--limit", "--runtime-dir"})},
            {"mutates", false},
            {"credential", "forbidden"},
            {"response", "uci_arena_provider_game_page_v1"},
            {"response_schema", "contracts/game-page-schema.json"},
        };
        operations["operations"]["game_get"] = {
            {"arguments",
             alternate_operation_prefixes
                 ? json::array({"match", "inspect"})
                 : json::array({"game", "get"})},
            {"required_arguments", json::array({"--game-id"})},
            {"optional_arguments", json::array({"--runtime-dir"})},
            {"mutates", false},
            {"credential", "forbidden"},
            {"response", "uci_arena_provider_game_v1"},
            {"response_schema", "contracts/game-detail-schema.json"},
        };
        manifest["contracts"]["game_page"] =
            "contracts/game-page-schema.json";
        manifest["contracts"]["game_detail"] =
            "contracts/game-detail-schema.json";
    }
    json service_contract{
        {"schema", "uci_arena_service_contract_v1"},
        {"service",
         {{"component_id", "lichess.adapter"},
          {"component_version", "1.0.0"},
          {"product_id", "fixture.provider"},
          {"contract_version", contract_version},
          {"transport", "cli_json"},
          {"entrypoint", entrypoint}}},
        {"invocation",
         direct_invocation
             ? json{{"kind", "direct"}}
             : json{{"kind", "dependency_runtime"},
                    {"runtime_component_id", "node_runtime.private"},
                    {"runtime_entrypoint", "node"}}},
        {"provided_capabilities", json::array({"fixture.status"})},
        {"required_capabilities", json::array({"fixture.runtime"})},
        {"credential_contract",
         {{"input", "external_absolute_file_path"},
          {"contents_visible_to", "provider_transport_only"},
          {"manager_projection", "path_and_redacted_status_only"},
          {"installer_behavior", "never_package_copy_or_inspect"}}},
        {"startup",
         {{"windows", "per_user_task_scheduler"},
          {"linux", "per_user_systemd"},
          {"credential_gated", true}}},
        {"persistence",
         {{"upgrade", "preserve_and_migrate"},
          {"repair", "preserve"},
          {"uninstall_default", "preserve"}}},
        {"manager_operations", "contracts/manager-operations.json"},
        {"configuration_schema", "contracts/configuration-schema.json"},
        {"configuration_update_schema",
         "contracts/configuration-update-schema.json"},
        {"status_schema", "contracts/status-schema.json"},
        {"error_contract",
         {{"format", "bounded_typed_json"},
          {"secrets", "redacted"},
          {"unknown_operation", "fail_closed"},
          {"unknown_major_version", "incompatible"}}},
    };
    if (provider_incidents) {
        service_contract["incident_page_schema"] =
            "contracts/incident-page-schema.json";
    }
    if (provider_games) {
        service_contract["game_page_schema"] =
            "contracts/game-page-schema.json";
        service_contract["game_detail_schema"] =
            "contracts/game-detail-schema.json";
    }
    std::vector<std::pair<std::string, json>> documents{
        {"contracts/service-contract.json", service_contract},
        {"contracts/manager-operations.json", operations},
        {"contracts/configuration-schema.json", json{{"schema", "fixture"}}},
        {"contracts/configuration-update-schema.json", json{{"schema", "fixture"}}},
        {"contracts/status-schema.json", json{{"schema", "fixture"}}},
    };
    if (provider_incidents) {
        documents.push_back({
            "contracts/incident-page-schema.json",
            {{"$id", "uci_arena_provider_incident_page_v1"},
             {"type", "object"},
             {"additionalProperties", false}},
        });
    }
    if (provider_games) {
        documents.push_back({
            "contracts/game-page-schema.json",
            {{"$id", "uci_arena_provider_game_page_v1"},
             {"type", "object"},
             {"additionalProperties", false}},
        });
        documents.push_back({
            "contracts/game-detail-schema.json",
            {{"$id", "uci_arena_provider_game_v1"},
             {"type", "object"},
             {"additionalProperties", false}},
        });
    }
    std::map<std::string, std::string> digests;
    for (const auto& [relative, document] : documents) {
        const std::string contents = document.dump(2) + '\n';
        fs::create_directories((active_root / relative).parent_path());
        std::ofstream(active_root / relative, std::ios::binary) << contents;
        const std::string digest = sha256_text(contents);
        digests[relative] = digest;
        manifest["files"].push_back(
            {{"path", relative},
             {"sha256", digest},
             {"size_bytes", contents.size()}});
    }
    write_json(manifest_path, manifest);
    receipt["service_contract"] = {
        {"schema", "uci_arena_service_contract_v1"},
        {"path", "contracts/service-contract.json"},
        {"sha256", digests.at("contracts/service-contract.json")},
        {"configuration_schema_path", "contracts/configuration-schema.json"},
        {"configuration_schema_sha256",
         digests.at("contracts/configuration-schema.json")},
        {"configuration_update_schema_path",
         "contracts/configuration-update-schema.json"},
        {"configuration_update_schema_sha256",
         digests.at("contracts/configuration-update-schema.json")},
        {"status_schema_path", "contracts/status-schema.json"},
        {"status_schema_sha256", digests.at("contracts/status-schema.json")},
        {"manager_operations_path", "contracts/manager-operations.json"},
        {"manager_operations_sha256",
         digests.at("contracts/manager-operations.json")},
        {"credential_gated_startup", true},
    };
    write_json(receipt_path, receipt);
}

void write_ready_bot_fixture(
    const fs::path& root,
    const std::string& node_entrypoint_name = "node",
    const bool direct_invocation = false,
    const bool alternate_operation_prefixes = false,
    const std::string& contract_version = "1.0",
    const bool installed_qualification = true,
    const bool provider_incidents = false,
    const bool provider_games = false) {
    const fs::path data = root / "data";
    const fs::path install = root / "install";
    write_installed_component(
        data,
        install,
        "node_runtime.private",
        node_entrypoint_name,
        "bin/node");
    write_installed_component(
        data,
        install,
        "uci_arena.engine",
        "uci",
        "bin/uci_arena_engine",
        {},
        {
            {"default_model",
             {
                 {"model_id", "qualified-model-v1"},
                 {"display_name", "Qualified Model"},
                 {"input_adapter_id", "qualified_dense_planes_v1"},
             }},
        });
    write_installed_component(
        data,
        install,
        "opening_book.standard",
        "book",
        "opening_books/default.bin");
    write_installed_component(
        data,
        install,
        "syzygy.wdl.3-4-5",
        "tablebase",
        "tablebases/KQvK.rtbw");
    write_installed_component(
        data,
        install,
        "lichess.adapter",
        "adapter",
        "app/adapter.mjs",
        {root / "workspaces" / "lichess-adapter"});
    attach_bot_provider_contract(
        data, direct_invocation, alternate_operation_prefixes,
        contract_version, installed_qualification, provider_incidents,
        provider_games);

    const std::array<std::pair<std::string, fs::path>, 5> locators{{
        {"timing_profile", root / "uci_time_profile_v3.json"},
        {"lichess_single_game_profile",
         root / "uci_arena_single_game_profile_v1.json"},
        {"lichess_capacity_profile",
         root / "uci_arena_lichess_capacity_profile_v2.json"},
        {"lichess_transport_profile", root / "transport_profile_v2.json"},
        {"lichess_token_path", root / "operator-token.txt"},
    }};
    for (const auto& [kind, path] : locators) {
        std::ofstream(path) << "fixture";
        write_json(
            data / "locators" / (kind + ".json"),
            {
                {"schema", "arena_saved_locator_v1"},
                {"schema_version", 1},
                {"kind", kind},
                {"mode", "in_place_reference"},
                {"path", path.string()},
                {"validation", json::object()},
            });
    }
    const fs::path configuration =
        root / "workspaces" / "lichess-adapter" / "manager-config.json";
    write_json(
        configuration,
        {
            {"engine",
             {
                 {"time_profile", locators.at(0).second.string()},
                 {"single_game_profile", locators.at(1).second.string()},
             }},
            {"capacity",
             {
                 {"capacity_profile", locators.at(2).second.string()},
                 {"total_slots", 1},
             }},
            {"timing",
             {{"transport_profile_path", locators.at(3).second.string()}}},
            {"challenges", {{"accept", true}}},
            {"scheduler", {{"enabled", false}}},
        });
    json receipt = read_json(data / "receipts" / "lichess.adapter.json");
    receipt["state"] = "installed_disabled";
    receipt["configuration_path"] = configuration.string();
    write_json(data / "receipts" / "lichess.adapter.json", receipt);
}

}  // namespace

TEST_CASE("UUIDv7 identifiers are valid and carry the version nibble") {
    const std::string id = arena::domain::uuid_v7();
    REQUIRE(arena::domain::is_uuid(id));
    REQUIRE(id.size() == 36);
    REQUIRE(id.at(14) == '7');
}

TEST_CASE("service catalog exposes both external services as optional and implemented") {
    const auto profiles = arena::application::ServiceCatalog::profiles();
    REQUIRE(profiles.size() == 2);
    REQUIRE(profiles.at(0).at("optional") == true);
    REQUIRE(profiles.at(1).at("optional") == true);
    REQUIRE(profiles.at(0).at("implementation_state") == "implemented_external");
    REQUIRE(profiles.at(1).at("implementation_state") == "implemented_external");
}

TEST_CASE("optional service discovery always exposes manual locator fallback") {
    const auto book =
        arena::application::ServiceCatalog::discover("book_forge");
    REQUIRE(book.at("optional") == true);
    REQUIRE(book.at("manual_configuration") ==
            nlohmann::json::array({"node_executable", "cli_entrypoint", "workspace"}));

    const auto evidence =
        arena::application::ServiceCatalog::discover("uci_arena_evidence");
    REQUIRE(evidence.at("manual_configuration") ==
            nlohmann::json::array({"node_executable", "cli_entrypoint", "workspace"}));
    REQUIRE_THROWS(
        arena::application::ServiceCatalog::normalize_registration(
            {{"profile_id", "uci_arena_evidence"}}));
}

TEST_CASE("service discovery consumes validated arena-setup locators without enabling them") {
    const fs::path root =
        fs::temp_directory_path() /
        ("arena-service-locator-" + arena::domain::uuid_v7());
    const fs::path data = root / "data";
    const fs::path provider = root / "Book Forge";
    fs::create_directories(data / "locators");
    fs::create_directories(provider / "app");
    const fs::path cli = provider / "app" / "book-forge.mjs";
    std::ofstream(cli) << "fixture";
    write_json(
        provider / "arena-component.json",
        {
            {"schema", "arena_install_component_v1"},
            {"component_id", "book_forge.service"},
            {"entrypoints", {{"cli", "app/book-forge.mjs"}}},
        });
    write_json(
        data / "locators" / "book_forge.json",
        {
            {"schema", "arena_saved_locator_v1"},
            {"schema_version", 1},
            {"kind", "book_forge"},
            {"mode", "in_place_reference"},
            {"path", provider.string()},
            {"validation", json::object()},
            {"saved_at", "2026-07-26T00:00:00Z"},
        });

    const auto discovered =
        arena::application::ServiceCatalog::discover("book_forge", data);
    REQUIRE(discovered.at("candidates").size() == 1);
    const auto& candidate = discovered.at("candidates").at(0);
    REQUIRE(candidate.at("source") == "saved_locator");
    REQUIRE(
        candidate.at("configuration").at("cli_entrypoint") ==
        fs::absolute(cli).lexically_normal().string());
    const auto missing =
        candidate.at("missing").get<std::vector<std::string>>();
    REQUIRE(
        std::find(missing.begin(), missing.end(), "workspace") !=
        missing.end());
    REQUIRE(candidate.at("complete") == false);
    fs::remove_all(root);
}

TEST_CASE("Evidence Service registration validates the built CLI process contract") {
    const auto directory =
        std::filesystem::temp_directory_path() /
        ("arena-evidence-profile-" + arena::domain::uuid_v7());
    std::filesystem::create_directories(directory);
    const auto node = directory / "node";
    const auto cli = directory / "cli.mjs";
    {
        std::ofstream(node) << "fixture";
        std::ofstream(cli) << "fixture";
    }
    const auto normalized = arena::application::ServiceCatalog::normalize_registration(
        {
            {"profile_id", "uci_arena_evidence"},
            {"node_executable", node.string()},
            {"cli_entrypoint", cli.string()},
            {"workspace", (directory / "workspace").string()},
        });
    REQUIRE(normalized.at("integration_state") == "configured");
    REQUIRE(normalized.at("observed_state") == "stopped");
    REQUIRE(normalized.at("transport") == "process");
    REQUIRE(normalized.at("health").at("state") == "unknown");
    REQUIRE(
        normalized.at("capabilities") ==
        json::array({
            "status", "start", "stop", "restart", "pause", "resume",
            "init", "target_add", "target_list", "target_remove",
            "job_enqueue", "job_list", "job_cancel", "job_retry",
            "dataset_list", "dataset_show", "dataset_export",
            "profile_list", "prune", "startup_install", "startup_status",
            "startup_uninstall"}));
    std::filesystem::remove(node);
    std::filesystem::remove(cli);
    std::filesystem::remove(directory);
}

TEST_CASE("installed service receipts reconcile exact private runtime entrypoints") {
    const fs::path root =
        fs::temp_directory_path() /
        ("arena-installed-services-" + arena::domain::uuid_v7());
    const fs::path data = root / "data";
    const fs::path install = root / "install";
    write_installed_component(
        data,
        install,
        "node_runtime.private",
        "executable",
        "bin/node");
    write_installed_component(
        data,
        install,
        "book_forge.service",
        "cli",
        "src/cli.mjs",
        {root / "workspaces" / "book-forge"},
        {{"manager_integration_api", manager_service_api()}});
    const fs::path service_receipt =
        data / "receipts" / "book_forge.service.json";
    const fs::path service_config =
        root / "workspaces" / "book-forge" / "manager-config.json";
    write_json(
        service_config,
        {
            {"schema", "uci_arena_book_forge_config_v1"},
            {"stockfish", "C:/verified/stockfish.exe"},
        });
    json receipt = read_json(service_receipt);
    receipt["configuration_path"] = service_config.string();
    receipt["startup"] = {{"installed", true}};
    write_json(service_receipt, receipt);

    const auto discovered =
        arena::application::ServiceCatalog::installed_registrations(data);
    REQUIRE(discovered.at("errors").empty());
    REQUIRE(discovered.at("services").size() == 1);
    REQUIRE(
        discovered.at("services").at(0).at("profile_id") == "book_forge");
    REQUIRE(
        discovered.at("services").at(0).at("runtime_component_id") ==
        "node_runtime.private");
    REQUIRE(
        discovered.at("services").at(0).at("config_path") ==
        fs::absolute(service_config).string());
    REQUIRE(
        discovered.at("services").at(0).at("desired_state") == "running");
    REQUIRE(
        discovered.at("services").at(0).at("startup_installed") == true);
    REQUIRE(
        std::find(
            discovered.at("services").at(0).at("capabilities").begin(),
            discovered.at("services").at(0).at("capabilities").end(),
            "startup_recover") !=
        discovered.at("services").at(0).at("capabilities").end());
    REQUIRE(
        discovered.at("services")
            .at(0)
            .at("manager_integration_api")
            .at("schema") ==
        "uci_arena_manager_service_api_v1");
    REQUIRE(
        discovered.at("services")
            .at(0)
            .at("manager_integration_api")
            .at("operations")
            .at(5) ==
        "acquisition_retry_auth");
    REQUIRE(
        discovered.at("services")
            .at(0)
            .at("manager_integration_api_source") ==
        "signed_component_manifest");

    const fs::path component_manifest =
        install / "book_forge.service" / "1.0.0" /
        "arena-component.json";
    json missing_api_manifest = read_json(component_manifest);
    missing_api_manifest.erase("manager_integration_api");
    write_json(component_manifest, missing_api_manifest);
    const auto missing_api =
        arena::application::ServiceCatalog::installed_registrations(data);
    REQUIRE(missing_api.at("services").empty());
    REQUIRE(missing_api.at("errors").size() == 1);
    missing_api_manifest["manager_integration_api"] =
        manager_service_api();
    write_json(component_manifest, missing_api_manifest);

    std::ofstream(
        install / "book_forge.service" / "1.0.0" / "unlisted.mjs")
        << "untrusted";
    const auto unlisted =
        arena::application::ServiceCatalog::installed_registrations(data);
    REQUIRE(unlisted.at("services").empty());
    REQUIRE(unlisted.at("errors").size() == 1);
    fs::remove(
        install / "book_forge.service" / "1.0.0" / "unlisted.mjs");

    std::ofstream(
        install / "book_forge.service" / "1.0.0" / "src" / "cli.mjs",
        std::ios::app)
        << "tamper";
    const auto tampered =
        arena::application::ServiceCatalog::installed_registrations(data);
    REQUIRE(tampered.at("services").empty());
    REQUIRE(tampered.at("errors").size() == 1);
    fs::remove_all(root);
}

TEST_CASE("installed reconciliation preserves one existing manual service profile") {
    const fs::path root =
        fs::temp_directory_path() /
        ("arena-installed-manual-service-" + arena::domain::uuid_v7());
    const fs::path data = root / "data";
    const fs::path install = root / "install";
    write_installed_component(
        data,
        install,
        "node_runtime.private",
        "executable",
        "bin/node");
    write_installed_component(
        data,
        install,
        "book_forge.service",
        "cli",
        "src/cli.mjs",
        {root / "workspaces" / "book-forge"},
        {{"manager_integration_api", manager_service_api()}});
    const fs::path service_receipt =
        data / "receipts" / "book_forge.service.json";
    const fs::path service_config =
        root / "workspaces" / "book-forge" / "manager-config.json";
    write_json(
        service_config,
        {
            {"schema", "uci_arena_book_forge_config_v1"},
            {"stockfish", "C:/verified/stockfish.exe"},
        });
    json receipt = read_json(service_receipt);
    receipt["configuration_path"] = service_config.string();
    receipt["startup"] = {{"installed", true}};
    write_json(service_receipt, receipt);

    const fs::path manual_node = root / "manual" / "node.exe";
    const fs::path manual_cli = root / "manual" / "book-cli.mjs";
    const fs::path manual_workspace = root / "manual-workspace";
    fs::create_directories(manual_node.parent_path());
    fs::create_directories(manual_workspace);
    std::ofstream(manual_node) << "mock";
    std::ofstream(manual_cli) << "mock";

    {
        auto store = std::make_shared<arena::adapters::SqliteStore>(
            root / "arena.db");
        arena::application::ControlPlane control_plane(store);
        control_plane.initialize();
        const json created = control_plane.submit(
            {
                {"schema_version", 1},
                {"request_id", arena::domain::uuid_v7()},
                {"idempotency_key", arena::domain::uuid_v7()},
                {"target", {{"kind", "managed_service"}, {"id", nullptr}}},
                {"action", "service.register"},
                {"dry_run", false},
                {"parameters",
                 {
                     {"profile_id", "book_forge"},
                     {"node_executable", manual_node.string()},
                     {"cli_entrypoint", manual_cli.string()},
                     {"workspace", manual_workspace.string()},
                 }},
            },
            "test");

        control_plane.reconcile_installed_services(data);
        const json resources = control_plane.list("managed_service");
        REQUIRE(resources.size() == 1);
        REQUIRE(resources.at(0).at("id") == created.at("resource").at("id"));
        REQUIRE(
            resources.at(0).at("revision") ==
            created.at("resource").at("revision"));
        REQUIRE_FALSE(
            resources.at(0).at("data").contains("installed_component_id"));
        const json changes =
            control_plane.status()
                .at("installed_service_reconciliation")
                .at("changes");
        REQUIRE(
            std::any_of(
                changes.begin(),
                changes.end(),
                [](const json& change) {
                    return change.value("state", "") ==
                           "manual_configuration_preserved";
                }));
    }
    fs::remove_all(root);
}

TEST_CASE("control-plane status reports the configured build version") {
    const fs::path root = fs::temp_directory_path() /
        ("arena-version-status-" + arena::domain::uuid_v7());
    {
        auto store = std::make_shared<arena::adapters::SqliteStore>(
            root / "arena.db");
        arena::application::ControlPlane control_plane(store);
        control_plane.initialize();
        REQUIRE(control_plane.status().at("version") == ARENA_VERSION);
    }
    fs::remove_all(root);
}

TEST_CASE("control-plane core readiness is available while reconciliation is queued") {
    const fs::path root = fs::temp_directory_path() /
        ("arena-core-readiness-" + arena::domain::uuid_v7());
    {
        auto store = std::make_shared<arena::adapters::SqliteStore>(
            root / "arena.db");
        arena::application::ControlPlane control_plane(store);
        control_plane.initialize();
        control_plane.begin_installed_reconciliation(root);

        const json readiness = control_plane.readiness();
        REQUIRE(readiness.at("state") == "core_ready");
        REQUIRE(
            readiness.at("installed_service_reconciliation").at("state") ==
            "pending");
        REQUIRE(
            readiness.at("installed_engine_reconciliation").at("state") ==
            "pending");
        REQUIRE(arena::application::manager_readiness_error(
                    readiness, ARENA_VERSION)
                    .empty());
    }
    fs::remove_all(root);
}

TEST_CASE("startup reconciliation cancellation skips pending external work") {
    const fs::path root = fs::temp_directory_path() /
        ("arena-reconciliation-cancel-" + arena::domain::uuid_v7());
    {
        auto store = std::make_shared<arena::adapters::SqliteStore>(
            root / "arena.db");
        arena::application::ControlPlane control_plane(store);
        control_plane.initialize();
        control_plane.begin_installed_reconciliation(root);
        std::stop_source stop;
        REQUIRE(stop.request_stop());

        control_plane.reconcile_installed_services(root, stop.get_token());

        const json status = control_plane.status();
        REQUIRE(
            status.at("installed_service_reconciliation").at("state") ==
            "complete_with_errors");
        REQUIRE(
            status.at("installed_service_reconciliation").at("phase") ==
            "cancelled");
        REQUIRE(
            status.at("installed_engine_reconciliation").at("state") ==
            "complete_with_errors");
        REQUIRE(
            status.at("installed_engine_reconciliation").at("phase") ==
            "cancelled");
    }
    fs::remove_all(root);
}

TEST_CASE("Manager reports render immutable verified artifacts without dry-run writes") {
    const fs::path root = fs::temp_directory_path() /
        ("arena-report-generation-" + arena::domain::uuid_v7());
    const fs::path data = root / "data";
    {
        auto store = std::make_shared<arena::adapters::SqliteStore>(
            data / "arena.db");
        arena::application::ControlPlane control_plane(store);
        control_plane.initialize();
        control_plane.reconcile_installed_services(data);

        const auto command = [](const bool dry_run) {
            return json{
                {"schema_version", 1},
                {"request_id", arena::domain::uuid_v7()},
                {"idempotency_key", arena::domain::uuid_v7()},
                {"target", {{"kind", "report"}, {"id", nullptr}}},
                {"action", "report.generate"},
                {"dry_run", dry_run},
                {"parameters",
                 {
                     {"display_name", "Acceptance report"},
                     {"scope", "current_state"},
                     {"competition_id", nullptr},
                     {"formats", {"html", "pdf", "csv", "json", "pgn"}},
                 }},
            };
        };

        const json preview = control_plane.submit(command(true), "test");
        REQUIRE(preview.at("dry_run") == true);
        REQUIRE(control_plane.list("report").empty());
        REQUIRE_FALSE(fs::exists(data / "reports"));

        const json generated = control_plane.submit(command(false), "test");
        const json& resource = generated.at("resource");
        const std::string id = resource.at("id");
        REQUIRE(resource.at("kind") == "report");
        REQUIRE(resource.at("data").at("artifacts").size() == 5);
        REQUIRE(control_plane.list("report").size() == 1);
        for (const auto& format : {"html", "pdf", "csv", "json", "pgn"}) {
            const auto artifact = control_plane.report_artifact(id, format);
            REQUIRE_FALSE(artifact.content.empty());
            REQUIRE(artifact.filename.find(id) != std::string::npos);
        }
        REQUIRE(control_plane.report_artifact(id, "pdf").content.starts_with("%PDF-"));

        std::ofstream(data / "reports" / id / "report.json", std::ios::app)
            << "tampered";
        try {
            static_cast<void>(control_plane.report_artifact(id, "json"));
            FAIL("tampered report artifact was served");
        } catch (const arena::application::CommandFailure& failure) {
            REQUIRE(failure.code() == "report_artifact_unavailable");
            REQUIRE(failure.http_status() == 409);
        }
    }
    fs::remove_all(root);
}

TEST_CASE("component inventory separates package and startup truth") {
    const fs::path root =
        fs::temp_directory_path() /
        ("arena-component-inventory-" + arena::domain::uuid_v7());
    const fs::path data = root / "data";
    const fs::path install = root / "install";
    write_installed_component(
        data, install, "book_forge.service", "cli", "app/book-forge.mjs");
    json receipt = read_json(data / "receipts" / "book_forge.service.json");
    receipt["startup"] = {{"installed", true}};
    write_json(data / "receipts" / "book_forge.service.json", receipt);

    const json inventory =
        arena::application::ServiceCatalog::component_inventory(data);
    const auto& components = inventory.at("components");
    const auto found = std::find_if(
        components.begin(), components.end(), [](const json& component) {
            return component.value("component_id", "") == "book_forge.service";
        });
    REQUIRE(found != components.end());
    REQUIRE(found->at("package_state") == "installed");
    REQUIRE(found->at("startup_installed") == true);
    const auto missing = std::find_if(
        components.begin(), components.end(), [](const json& component) {
            return component.value("component_id", "") ==
                   "uci_arena_evidence.service";
        });
    REQUIRE(missing != components.end());
    REQUIRE(missing->at("package_state") == "missing");
    fs::remove_all(root);
}

TEST_CASE("Lichess bot discovery projects every missing readiness dimension") {
    const fs::path root =
        fs::temp_directory_path() /
        ("arena-bot-discovery-" + arena::domain::uuid_v7());
    const fs::path data = root / "data";
    const fs::path install = root / "install";
    write_installed_component(
        data, install, "node_runtime.private", "node", "bin/node");
    write_installed_component(
        data, install, "lichess.adapter", "adapter", "app/adapter.mjs",
        {root / "workspaces" / "lichess-adapter"});
    attach_bot_provider_contract(data);
    const fs::path configuration =
        root / "workspaces" / "lichess-adapter" / "manager-config.json";
    write_json(
        configuration,
        {
            {"capacity", {{"total_slots", 1}}},
            {"challenges", {{"accept", true}}},
            {"scheduler", {{"enabled", false}}},
        });
    json receipt = read_json(data / "receipts" / "lichess.adapter.json");
    receipt["state"] = "installed_disabled";
    receipt["configuration_path"] = configuration.string();
    write_json(data / "receipts" / "lichess.adapter.json", receipt);
    const fs::path token = root / "operator-token.txt";
    const std::string sentinel = "TOKEN_VALUE_MUST_NOT_BE_READ";
    std::ofstream(token) << sentinel;
    write_json(
        data / "locators" / "lichess_token_path.json",
        {
            {"schema", "arena_saved_locator_v1"},
            {"schema_version", 1},
            {"kind", "lichess_token_path"},
            {"mode", "in_place_reference"},
            {"path", token.string()},
            {"validation", {{"validation", "path_reference_only"}}},
        });

    const json discovered =
        arena::application::ServiceCatalog::discover_bot(data);
    const json& candidate = discovered.at("candidates").at(0);
    REQUIRE(candidate.at("state") == "ready_to_probe");
    REQUIRE(candidate.at("startup_eligibility") == "blocked");
    REQUIRE(candidate.at("package_state") == "installed");
    REQUIRE(candidate.at("token_reference_available") == true);
    REQUIRE(candidate.at("options_state") == "interactive_helper_required");
    REQUIRE_FALSE(candidate.contains("options"));
    REQUIRE(candidate.at("readiness").at("verified_adapter") == "ready");
    REQUIRE(candidate.at("readiness").at("adapter_runtime") == "ready");
    REQUIRE(
        candidate.at("readiness").at("verified_uci_arena_engine") ==
        "missing");
    REQUIRE(candidate.at("readiness").at("active_model") == "missing");
    REQUIRE(candidate.at("readiness").at("opening_book") == "missing");
    REQUIRE(candidate.at("readiness").at("syzygy") == "missing");
    REQUIRE(candidate.at("readiness").at("timing_profile") == "missing");
    REQUIRE(candidate.at("readiness").at("single_game_profile") == "missing");
    REQUIRE(candidate.at("readiness").at("capacity_profile") == "missing");
    REQUIRE(candidate.at("readiness").at("transport_profile") == "bootstrap");
    REQUIRE(candidate.at("readiness").at("token_reference") == "ready");
    REQUIRE(
        candidate.at("readiness").at("bot_account_probe") ==
        "not_verified");
    REQUIRE(candidate.at("readiness").at("startup") == "disabled");
    REQUIRE(candidate.at("issues").dump().find(
                "bot_transport_profile_unavailable") == std::string::npos);
    REQUIRE(discovered.dump().find(sentinel) == std::string::npos);
    std::size_t probe_calls = 0;
    const json probe = arena::application::ServiceCatalog::probe_bot(
        data,
        [&probe_calls](
            const fs::path&,
            const fs::path&,
            const std::vector<std::string>&,
            const std::chrono::milliseconds) {
            ++probe_calls;
            return json{
                {"status", "ready"},
                {"account", "EngineIndependentBot"},
                {"is_bot", true},
                {"games", 0U},
            };
        });
    REQUIRE(probe.at("account_name") == "EngineIndependentBot");
    REQUIRE(probe_calls == 1);
    std::size_t startup_calls = 0;
    try {
        static_cast<void>(
            arena::application::ServiceCatalog::control_bot(
                data,
                [&startup_calls](
                    const fs::path&,
                    const fs::path&,
                    const std::vector<std::string>&,
                    const std::chrono::milliseconds) {
                    ++startup_calls;
                    return json::object();
                },
                "enable_startup"));
        FAIL("startup must remain blocked without live-play readiness");
    } catch (const std::runtime_error& error) {
        REQUIRE(
            std::string(error.what()).find("live-play readiness") !=
            std::string::npos);
    }
    REQUIRE(startup_calls == 0);
    fs::remove_all(root);
}

TEST_CASE("Lichess bot discovery becomes probe-ready only with exact visible prerequisites") {
    const fs::path root =
        fs::temp_directory_path() /
        ("arena-bot-ready-" + arena::domain::uuid_v7());
    const fs::path data = root / "data";
    const fs::path install = root / "install";
    write_installed_component(
        data, install, "node_runtime.private", "node", "bin/node");
    write_installed_component(
        data,
        install,
        "uci_arena.engine",
        "uci",
        "bin/uci_arena_engine",
        {},
        {
            {"default_model",
             {
                 {"model_id", "qualified-model-v1"},
                 {"display_name", "Qualified Model"},
                 {"input_adapter_id", "qualified_dense_planes_v1"},
             }},
        });
    write_installed_component(
        data,
        install,
        "opening_book.standard",
        "book",
        "opening_books/default.bin");
    write_installed_component(
        data,
        install,
        "syzygy.wdl.3-4-5",
        "tablebase",
        "tablebases/KQvK.rtbw");
    write_installed_component(
        data,
        install,
        "lichess.adapter",
        "adapter",
        "app/adapter.mjs",
        {root / "workspaces" / "lichess-adapter"});
    attach_bot_provider_contract(data);

    const fs::path timing = root / "uci_time_profile_v3.json";
    const fs::path single = root / "uci_arena_single_game_profile_v1.json";
    const fs::path capacity =
        root / "uci_arena_lichess_capacity_profile_v2.json";
    const fs::path transport = root / "transport_profile_v2.json";
    const fs::path token = root / "operator-token.txt";
    for (const auto& path : {timing, single, capacity, transport, token}) {
        std::ofstream(path) << "fixture";
    }
    const auto write_locator = [&data](
                                   const std::string& kind,
                                   const fs::path& path) {
        write_json(
            data / "locators" / (kind + ".json"),
            {
                {"schema", "arena_saved_locator_v1"},
                {"schema_version", 1},
                {"kind", kind},
                {"mode", "in_place_reference"},
                {"path", path.string()},
                {"validation", json::object()},
            });
    };
    write_locator("timing_profile", timing);
    write_locator("lichess_single_game_profile", single);
    write_locator("lichess_capacity_profile", capacity);
    write_locator("lichess_transport_profile", transport);
    write_locator("lichess_token_path", token);

    const fs::path configuration =
        root / "workspaces" / "lichess-adapter" / "manager-config.json";
    write_json(
        configuration,
        {
            {"engine",
             {
                 {"time_profile", timing.string()},
                 {"single_game_profile", single.string()},
             }},
            {"capacity",
             {
                 {"capacity_profile", capacity.string()},
                 {"total_slots", 1},
             }},
            {"timing", {{"transport_profile_path", transport.string()}}},
            {"challenges", {{"accept", true}}},
            {"scheduler", {{"enabled", false}}},
        });
    json receipt = read_json(data / "receipts" / "lichess.adapter.json");
    receipt["state"] = "installed_disabled";
    receipt["configuration_path"] = configuration.string();
    write_json(data / "receipts" / "lichess.adapter.json", receipt);

    const json configured_candidate =
        arena::application::ServiceCatalog::discover_bot(data)
            .at("candidates")
            .at(0);
    REQUIRE(
        configured_candidate.at("readiness").at("capacity_profile") ==
        "configured");
    REQUIRE(configured_candidate.at("startup_eligibility") == "blocked");

    const json candidate =
        arena::application::ServiceCatalog::discover_bot(
            data,
            [](const fs::path&,
               const fs::path&,
               const std::vector<std::string>&,
               const std::chrono::milliseconds) {
                return manager_bot_options_fixture();
            })
            .at("candidates")
            .at(0);
    REQUIRE(candidate.at("state") == "ready_to_probe");
    REQUIRE(candidate.at("startup_eligibility") == "ready");
    REQUIRE(candidate.at("options_state") == "interactive_helper_required");
    REQUIRE_FALSE(candidate.contains("options"));
    for (const auto& dimension :
         {"verified_adapter", "adapter_runtime",
          "verified_uci_arena_engine", "active_model", "opening_book",
          "syzygy", "timing_profile", "single_game_profile",
          "capacity_profile", "token_reference"}) {
        REQUIRE(candidate.at("readiness").at(dimension) == "ready");
    }
    REQUIRE(
        candidate.at("readiness").at("transport_profile") ==
        "configured");
    REQUIRE(
        candidate.at("readiness").at("bot_account_probe") ==
        "not_verified");
    REQUIRE(candidate.at("readiness").at("startup") == "disabled");
    REQUIRE(
        candidate.at("default_model").at("display_name") ==
        "Qualified Model");

    const json rejected =
        arena::application::ServiceCatalog::discover_bot(
            data,
            [](const fs::path&,
               const fs::path&,
               const std::vector<std::string>&,
               const std::chrono::milliseconds) {
                json options = manager_bot_options_fixture();
                options["capacity"]["evidence"] = {
                    {"status", "rejected"},
                    {"qualified", false},
                    {"maximum_concurrency", nullptr},
                };
                return options;
            })
            .at("candidates")
            .at(0);
    REQUIRE(rejected.at("readiness").at("capacity_profile") == "configured");
    REQUIRE(rejected.at("startup_eligibility") == "blocked");
    REQUIRE(
        rejected.at("issues").dump().find(
            "bot_capacity_profile_unqualified") != std::string::npos);

    std::size_t rejected_startup_calls = 0;
    try {
        static_cast<void>(arena::application::ServiceCatalog::control_bot(
            data,
            [&rejected_startup_calls](
                const fs::path&,
                const fs::path&,
                const std::vector<std::string>&,
                const std::chrono::milliseconds) {
                ++rejected_startup_calls;
                json options = manager_bot_options_fixture();
                options["capacity"]["evidence"] = {
                    {"status", "rejected"},
                    {"qualified", false},
                    {"maximum_concurrency", nullptr},
                };
                return options;
            },
            "enable_startup"));
        FAIL("rejected provider capacity must block startup");
    } catch (const std::runtime_error& error) {
        REQUIRE(
            std::string(error.what()).find("live-play readiness") !=
            std::string::npos);
    }
    REQUIRE(rejected_startup_calls == 1);
    fs::remove_all(root);
}

TEST_CASE("Lichess bot discovery exposes an incompatible provider contract major") {
    const fs::path root =
        fs::temp_directory_path() /
        ("arena-bot-contract-major-" + arena::domain::uuid_v7());
    write_ready_bot_fixture(root, "node", false, false, "2.0");

    const json candidate =
        arena::application::ServiceCatalog::discover_bot(root / "data")
            .at("candidates")
            .at(0);
    REQUIRE(candidate.at("package_state") == "installed");
    REQUIRE(candidate.at("integration_state") == "incompatible");
    REQUIRE(candidate.at("readiness").at("verified_adapter") == "missing");
    REQUIRE(candidate.at("issues").dump().find("bot_adapter_incompatible") !=
            std::string::npos);
    fs::remove_all(root);
}

TEST_CASE("Lichess providers without the additive qualification operation remain compatible") {
    const fs::path root =
        fs::temp_directory_path() /
        ("arena-bot-qualification-optional-" + arena::domain::uuid_v7());
    write_ready_bot_fixture(root, "node", false, false, "1.0", false);
    const json candidate =
        arena::application::ServiceCatalog::discover_bot(root / "data")
            .at("candidates")
            .at(0);
    REQUIRE(candidate.at("integration_state") == "compatible");
    REQUIRE(candidate.at("qualification_state") == "unavailable");
    REQUIRE_THROWS(
        arena::application::ServiceCatalog::qualify_bot(
            root / "data",
            [](const fs::path&,
               const fs::path&,
               const std::vector<std::string>&,
               const std::chrono::milliseconds) {
                return json::object();
            }));
    fs::remove_all(root);
}

TEST_CASE("Lichess installed qualification uses only the published provider operation and retained workspace") {
    const fs::path root =
        fs::temp_directory_path() /
        ("arena-bot-qualification-" + arena::domain::uuid_v7());
    write_ready_bot_fixture(root, "node", false, true);
    const fs::path runtime = root / "workspaces" / "lichess-adapter";
    const fs::path outputs = runtime / "qualification" / "fixture";
    fs::create_directories(outputs);
    const fs::path single =
        outputs / "uci_arena_single_game_profile_v1.json";
    const fs::path capacity =
        outputs / "uci_arena_lichess_capacity_profile_v2.json";
    const std::string single_contents{"single-game\n"};
    const std::string capacity_contents{"capacity\n"};
    std::ofstream(single, std::ios::binary) << single_contents;
    std::ofstream(capacity, std::ios::binary) << capacity_contents;
    std::vector<std::string> observed_arguments;
    std::chrono::milliseconds observed_timeout{};

    const json result = arena::application::ServiceCatalog::qualify_bot(
        root / "data",
        [&](const fs::path&,
            const fs::path&,
            const std::vector<std::string>& arguments,
            const std::chrono::milliseconds timeout) {
            observed_arguments = arguments;
            observed_timeout = timeout;
            return json{
                {"schema", "uci_arena_lichess_qualification_result_v1"},
                {"status", "qualified"},
                {"engine_sha256", "sha256:" + std::string(64, 'a')},
                {"timing_sha256", "sha256:" + std::string(64, 'b')},
                {"timing_identity_sha256", "sha256:" + std::string(64, 'c')},
                {"maximum_concurrency", 2},
                {"single_game_profile", single.string()},
                {"single_game_profile_sha256",
                 "sha256:" + sha256_text(single_contents)},
                {"capacity_profile", capacity.string()},
                {"capacity_profile_sha256",
                 "sha256:" + sha256_text(capacity_contents)},
                {"search_core", "mcgs"},
                {"model_id", "qualified-model-v1"},
                {"provider", "directml"},
                {"completed_at", "2026-08-08T00:00:00.000Z"},
                {"reused", false},
                {"additive_provider_field", "MUST_NOT_ESCAPE"},
            };
        });

    REQUIRE(result.at("schema") == "arena_bot_qualification_internal_v1");
    REQUIRE(
        arena::application::ServiceCatalog::discover_bot(root / "data")
            .at("candidates")
            .at(0)
            .at("qualification_state") == "available");
    REQUIRE(fs::equivalent(
        fs::path(result.at("single_game_profile").get<std::string>()), single));
    REQUIRE(fs::equivalent(
        fs::path(result.at("capacity_profile").get<std::string>()), capacity));
    REQUIRE(result.dump().find("MUST_NOT_ESCAPE") == std::string::npos);
    REQUIRE(observed_timeout == std::chrono::minutes(20));
    REQUIRE(observed_arguments.size() == 7);
    REQUIRE(observed_arguments.at(1) == "evidence");
    REQUIRE(observed_arguments.at(2) == "refresh");
    REQUIRE(observed_arguments.at(3) == "--config");
    REQUIRE(observed_arguments.at(5) == "--runtime-dir");
    REQUIRE(fs::equivalent(fs::path(observed_arguments.at(6)), runtime));
    REQUIRE(
        std::find(
            observed_arguments.begin(), observed_arguments.end(), "--token-file") ==
        observed_arguments.end());
    fs::remove_all(root);
}

TEST_CASE("Lichess installed qualification projects only a closed provider failure") {
    const fs::path root =
        fs::temp_directory_path() /
        ("arena-bot-qualification-failure-" + arena::domain::uuid_v7());
    write_ready_bot_fixture(root, "node", false, true);

    try {
        static_cast<void>(arena::application::ServiceCatalog::qualify_bot(
            root / "data",
            [](const fs::path&,
               const fs::path&,
               const std::vector<std::string>&,
               const std::chrono::milliseconds) {
                return json{
                    {"schema", "uci_arena_lichess_qualification_result_v1"},
                    {"status", "failed"},
                    {"failure",
                     {
                         {"code", "qualification_complete_game_failed"},
                         {"stage", "complete_game"},
                         {"detail",
                          "Complete-game matrix rejected cache authority"},
                     }},
                };
            }));
        FAIL("provider qualification failure was accepted as success");
    } catch (const arena::application::BotQualificationFailure& error) {
        REQUIRE(error.code() == "qualification_complete_game_failed");
        REQUIRE(error.stage() == "complete_game");
        REQUIRE(
            error.detail() ==
            "Complete-game matrix rejected cache authority");
    }
    fs::remove_all(root);
}

TEST_CASE("Lichess installed qualification rejects path-bearing provider diagnostics") {
    const fs::path root =
        fs::temp_directory_path() /
        ("arena-bot-qualification-redaction-" + arena::domain::uuid_v7());
    write_ready_bot_fixture(root, "node", false, true);

    try {
        static_cast<void>(arena::application::ServiceCatalog::qualify_bot(
            root / "data",
            [](const fs::path&,
               const fs::path&,
               const std::vector<std::string>&,
               const std::chrono::milliseconds) {
                return json{
                    {"schema", "uci_arena_lichess_qualification_result_v1"},
                    {"status", "failed"},
                    {"failure",
                     {
                         {"code", "qualification_preflight_failed"},
                         {"stage", "preflight"},
                         {"detail", "failed at C:\\private\\engine.exe"},
                     }},
                };
            }));
        FAIL("path-bearing provider diagnostic escaped validation");
    } catch (const arena::application::BotQualificationFailure&) {
        FAIL("invalid provider diagnostic became operator-visible");
    } catch (const std::runtime_error& error) {
        REQUIRE(
            std::string(error.what()) ==
            "the provider returned an invalid qualification failure");
    }
    fs::remove_all(root);
}

TEST_CASE("Lichess installed qualification rejects output outside the provider workspace") {
    const fs::path root =
        fs::temp_directory_path() /
        ("arena-bot-qualification-escape-" + arena::domain::uuid_v7());
    write_ready_bot_fixture(root);
    const fs::path outside_single =
        root / "uci_arena_single_game_profile_v1.json";
    const fs::path runtime = root / "workspaces" / "lichess-adapter";
    const fs::path capacity =
        runtime / "uci_arena_lichess_capacity_profile_v2.json";
    const std::string single_contents{"single-game\n"};
    const std::string capacity_contents{"capacity\n"};
    std::ofstream(outside_single, std::ios::binary) << single_contents;
    std::ofstream(capacity, std::ios::binary) << capacity_contents;
    try {
        static_cast<void>(arena::application::ServiceCatalog::qualify_bot(
            root / "data",
            [&](const fs::path&,
                const fs::path&,
                const std::vector<std::string>&,
                const std::chrono::milliseconds) {
                return json{
                    {"schema", "uci_arena_lichess_qualification_result_v1"},
                    {"status", "qualified"},
                    {"engine_sha256", "sha256:" + std::string(64, 'a')},
                    {"timing_sha256", "sha256:" + std::string(64, 'b')},
                    {"timing_identity_sha256", "sha256:" + std::string(64, 'c')},
                    {"maximum_concurrency", 1},
                    {"single_game_profile", outside_single.string()},
                    {"single_game_profile_sha256",
                     "sha256:" + sha256_text(single_contents)},
                    {"capacity_profile", capacity.string()},
                    {"capacity_profile_sha256",
                     "sha256:" + sha256_text(capacity_contents)},
                    {"search_core", "mcgs"},
                    {"model_id", "qualified-model-v1"},
                    {"provider", "directml"},
                    {"completed_at", "2026-08-08T00:00:00.000Z"},
                    {"reused", false},
                };
            }));
        FAIL("qualification output outside the provider workspace was accepted");
    } catch (const std::runtime_error& error) {
        REQUIRE(
            std::string(error.what()) ==
            "the provider qualification output escaped its retained workspace");
    }
    fs::remove_all(root);
}

TEST_CASE("Lichess BOT control re-probes identity and returns only sanitized startup state") {
    const fs::path root =
        fs::temp_directory_path() /
        ("arena-bot-control-" + arena::domain::uuid_v7());
    write_ready_bot_fixture(root);
    fs::remove(
        root / "data" / "locators" /
        "lichess_transport_profile.json");
    fs::remove(root / "transport_profile_v2.json");
    const fs::path configuration =
        root / "workspaces" / "lichess-adapter" / "manager-config.json";
    json provider_configuration = read_json(configuration);
    provider_configuration["timing"]["transport_profile_path"] = nullptr;
    write_json(configuration, provider_configuration);
    const json bootstrap_candidate =
        arena::application::ServiceCatalog::discover_bot(
            root / "data",
            [](const fs::path&,
               const fs::path&,
               const std::vector<std::string>&,
               const std::chrono::milliseconds) {
                return manager_bot_options_fixture();
            })
            .at("candidates")
            .at(0);
    REQUIRE(
        bootstrap_candidate.at("readiness").at("transport_profile") ==
        "bootstrap");
    REQUIRE(bootstrap_candidate.at("startup_eligibility") == "ready");
    std::vector<std::vector<std::string>> calls;
    const auto runner =
        [&calls](
            const fs::path&,
            const fs::path&,
            const std::vector<std::string>& arguments,
            const std::chrono::milliseconds) {
            calls.push_back(arguments);
            if (arguments.size() > 2 && arguments.at(1) == "config" &&
                arguments.at(2) == "get") {
                return manager_bot_options_fixture();
            }
            if (arguments.size() > 1 && arguments.at(1) == "probe") {
                return json{
                    {"status", "ready"},
                    {"account", "VerifiedArenaBot"},
                    {"is_bot", true},
                    {"games", 17U},
                    {"authorization", "MUST_NOT_ESCAPE"},
                    {"token_path", "MUST_NOT_ESCAPE"},
                };
            }
            if (arguments.size() > 1 && arguments.at(1) == "status") {
                return json{
                    {"schema", "uci_arena_lichess_status_v3"},
                    {"active", true},
                    {"ready", true},
                    {"stream_connected", true},
                    {"blockers", json::array()},
                    {"token", "MUST_NOT_ESCAPE"},
                };
            }
            return json{
                {"status", "ready"},
                {"startup",
                 {
                     {"installed", true},
                     {"started", true},
                     {"taskStatus", "Running"},
                     {"command", "MUST_NOT_ESCAPE"},
                     {"unitPath", "MUST_NOT_ESCAPE"},
                 }},
            };
        };

    const json result = arena::application::ServiceCatalog::control_bot(
        root / "data", runner, "enable_startup");
    REQUIRE(result.at("schema") == "arena_bot_control_result_v1");
    REQUIRE(result.at("state") == "active");
    REQUIRE(result.at("account_name") == "VerifiedArenaBot");
    REQUIRE(result.dump().find("MUST_NOT_ESCAPE") == std::string::npos);
    REQUIRE(result.at("provider_state") == "ready");
    REQUIRE(result.at("provider_ready") == true);
    REQUIRE(calls.size() == 4);
    REQUIRE(calls.at(0).at(1) == "config");
    REQUIRE(calls.at(0).at(2) == "get");
    REQUIRE(calls.at(1).at(1) == "probe");
    REQUIRE(calls.at(2).at(1) == "startup");
    REQUIRE(calls.at(2).at(2) == "install");
    REQUIRE(calls.at(3).at(1) == "status");
    REQUIRE(
        std::find(
            calls.at(2).begin(),
            calls.at(2).end(),
            "VerifiedArenaBot") != calls.at(2).end());
    fs::remove_all(root);
}

TEST_CASE("Lichess BOT status and disable never require credential arguments") {
    const fs::path root =
        fs::temp_directory_path() /
        ("arena-bot-disable-" + arena::domain::uuid_v7());
    write_ready_bot_fixture(root, "executable");
    std::vector<std::vector<std::string>> calls;
    const auto runner =
        [&calls](
            const fs::path&,
            const fs::path&,
            const std::vector<std::string>& arguments,
            const std::chrono::milliseconds) {
            calls.push_back(arguments);
            if (arguments.size() > 1 && arguments.at(1) == "status") {
                return json{
                    {"schema", "uci_arena_lichess_status_v3"},
                    {"active", false},
                    {"ready", false},
                    {"stream_connected", false},
                    {"blockers", json::array({"provider_process_inactive"})},
                };
            }
            const bool uninstall =
                arguments.size() > 2 && arguments.at(2) == "uninstall";
            return json{
                {"status", "ready"},
                {"startup",
                 {
                     {"installed", !uninstall},
                     {"taskStatus", "Ready"},
                 }},
            };
        };

    const json status = arena::application::ServiceCatalog::control_bot(
        root / "data", runner, "status");
    const json disabled = arena::application::ServiceCatalog::control_bot(
        root / "data", runner, "disable_startup");
    REQUIRE(status.at("state") == "enabled");
    REQUIRE(status.at("active") == false);
    REQUIRE(disabled.at("state") == "disabled");
    REQUIRE(status.at("provider_state") == "inactive");
    REQUIRE(disabled.at("provider_state") == "inactive");
    REQUIRE(calls.size() == 4);
    for (const auto& arguments : calls) {
        REQUIRE(
            std::find(arguments.begin(), arguments.end(), "--token-file") ==
            arguments.end());
        REQUIRE(
            std::find(
                arguments.begin(),
                arguments.end(),
                "--expected-account") == arguments.end());
    }
    REQUIRE_THROWS_AS(
        arena::application::ServiceCatalog::control_bot(
            root / "data", runner, "register"),
        std::invalid_argument);
    fs::remove_all(root);
}

TEST_CASE("Lichess BOT recovery preserves the provider reconciliation blocker") {
    const fs::path root =
        fs::temp_directory_path() /
        ("arena-bot-recovery-" + arena::domain::uuid_v7());
    write_ready_bot_fixture(root, "executable");
    std::vector<std::vector<std::string>> calls;
    const auto runner =
        [&calls](
            const fs::path&,
            const fs::path&,
            const std::vector<std::string>& arguments,
            const std::chrono::milliseconds) {
            calls.push_back(arguments);
            if (arguments.size() > 2 && arguments.at(1) == "startup" &&
                arguments.at(2) == "recover") {
                return json{
                    {"status", "ready"},
                    {"startup",
                     {{"installed", true},
                      {"recoveryRequested", true},
                      {"registrationMutated", false}}},
                };
            }
            return json{
                {"schema", "uci_arena_lichess_status_v3"},
                {"active", true},
                {"ready", false},
                {"stream_connected", false},
                {"blockers",
                 json::array({"startup_reconciliation_pending"})},
            };
        };

    const json result = arena::application::ServiceCatalog::control_bot(
        root / "data", runner, "recover_startup");
    REQUIRE(result.at("installed") == true);
    REQUIRE(result.at("active") == true);
    REQUIRE(result.at("provider_ready") == false);
    REQUIRE(result.at("recovery_requested") == true);
    REQUIRE(
        result.at("blockers") ==
        json::array({"startup_reconciliation_pending"}));
    REQUIRE(calls.size() == 2);
    REQUIRE(calls.at(0).at(1) == "startup");
    REQUIRE(calls.at(0).at(2) == "recover");
    for (const auto& arguments : calls) {
        REQUIRE(
            std::find(arguments.begin(), arguments.end(), "--token-file") ==
            arguments.end());
    }
    fs::remove_all(root);
}

TEST_CASE("Lichess BOT options use only verified provider entrypoints and bounded projections") {
    const fs::path root =
        fs::temp_directory_path() /
        ("arena-bot-options-" + arena::domain::uuid_v7());
    write_ready_bot_fixture(root);
    std::vector<std::vector<std::string>> calls;
    const auto result = arena::application::ServiceCatalog::bot_options(
        root / "data",
        [&calls](
            const fs::path& executable,
            const fs::path& working_directory,
            const std::vector<std::string>& arguments,
            const std::chrono::milliseconds timeout) {
            REQUIRE(fs::is_regular_file(executable));
            REQUIRE(fs::is_directory(working_directory));
            REQUIRE(timeout == std::chrono::seconds(30));
            calls.push_back(arguments);
            return manager_bot_options_fixture();
        });

    REQUIRE(result.at("schema") == "uci_arena_lichess_manager_options_v1");
    REQUIRE(result.at("capacity").at("evidence").at("qualified") == true);
    REQUIRE(result.at("scheduler").at("first_move_timeout_seconds") == 60);
    REQUIRE(calls.size() == 1);
    REQUIRE(calls.at(0).at(1) == "config");
    REQUIRE(calls.at(0).at(2) == "get");
    REQUIRE(calls.at(0).at(3) == "--config");
    REQUIRE_THROWS_AS(
        arena::application::ServiceCatalog::bot_options(
            root / "data",
            [](const fs::path&,
               const fs::path&,
               const std::vector<std::string>&,
               const std::chrono::milliseconds) {
                json invalid = manager_bot_options_fixture();
                invalid["token"] = "MUST_NOT_ESCAPE";
                return invalid;
            }),
        std::runtime_error);
    REQUIRE_THROWS_AS(
        arena::application::ServiceCatalog::bot_options(
            root / "data",
            [](const fs::path&,
               const fs::path&,
               const std::vector<std::string>&,
               const std::chrono::milliseconds) {
                json invalid = manager_bot_options_fixture();
                invalid["scheduler"]["first_move_timeout_seconds"] = 29;
                return invalid;
            }),
        std::runtime_error);
    fs::remove_all(root);
}

TEST_CASE("conforming direct provider substitutes its declared operation prefixes") {
    const fs::path root =
        fs::temp_directory_path() /
        ("arena-bot-provider-substitution-" + arena::domain::uuid_v7());
    write_ready_bot_fixture(root, "node", true, true);
    fs::path observed_executable;
    std::vector<std::string> observed_arguments;
    const json result = arena::application::ServiceCatalog::bot_options(
        root / "data",
        [&observed_executable, &observed_arguments](
            const fs::path& executable,
            const fs::path&,
            const std::vector<std::string>& arguments,
            const std::chrono::milliseconds) {
            observed_executable = executable;
            observed_arguments = arguments;
            return manager_bot_options_fixture();
        });
    REQUIRE(result.at("schema") == "uci_arena_lichess_manager_options_v1");
    REQUIRE(observed_executable.filename() == "adapter.mjs");
    REQUIRE(observed_arguments.at(0) == "settings");
    REQUIRE(observed_arguments.at(1) == "show");
    REQUIRE(observed_arguments.at(2) == "--config");
    fs::remove_all(root);
}

TEST_CASE("generic provider status projects a bounded receipt-declared status-v3 operation") {
    const fs::path root =
        fs::temp_directory_path() /
        ("arena-provider-status-" + arena::domain::uuid_v7());
    write_ready_bot_fixture(root, "node", true, true);
    fs::path observed_executable;
    fs::path observed_working_directory;
    std::vector<std::string> observed_arguments;
    const json result = arena::application::ServiceCatalog::provider_status(
        root / "data",
        [&observed_executable, &observed_working_directory, &observed_arguments](
            const fs::path& executable,
            const fs::path& working_directory,
            const std::vector<std::string>& arguments,
            const std::chrono::milliseconds timeout) {
            observed_executable = executable;
            observed_working_directory = working_directory;
            observed_arguments = arguments;
            REQUIRE(timeout == std::chrono::seconds(30));
            return provider_status_v3_fixture();
        },
        "lichess_bot");

    REQUIRE(result.at("schema") == "arena_provider_status_projection_v1");
    REQUIRE(result.at("profile_id") == "lichess_bot");
    REQUIRE(result.at("provider_contract_version") == "1.0");
    REQUIRE(result.at("supervision_availability") == "unavailable");
    REQUIRE(result.at("incident_availability") == "unavailable");
    REQUIRE(result.at("status").at("scheduler").at("fault").at("status") == 429);
    REQUIRE(result.at("status").at("rate_limit").at("state") == "cooldown");
    REQUIRE(observed_executable.filename() == "adapter.mjs");
    REQUIRE(fs::is_directory(observed_working_directory));
    REQUIRE(observed_arguments.size() == 4);
    REQUIRE(observed_arguments.at(0) == "provider");
    REQUIRE(observed_arguments.at(1) == "observe");
    REQUIRE(observed_arguments.at(2) == "--runtime-dir");
    REQUIRE(fs::is_directory(observed_arguments.at(3)));

    const auto invalid_runner = [](
                                    const fs::path&,
                                    const fs::path&,
                                    const std::vector<std::string>&,
                                    const std::chrono::milliseconds) {
        json invalid = provider_status_v3_fixture();
        invalid["token"] = "MUST_NOT_ESCAPE";
        return invalid;
    };
    REQUIRE_THROWS_AS(
        arena::application::ServiceCatalog::provider_status(
            root / "data", invalid_runner, "lichess_bot"),
        std::runtime_error);
    REQUIRE_THROWS_AS(
        arena::application::ServiceCatalog::provider_status(
            root / "data",
            [](const fs::path&,
               const fs::path&,
               const std::vector<std::string>&,
               const std::chrono::milliseconds) {
                json invalid = provider_status_v3_fixture();
                invalid["rate_limit"]["queue_depth"] =
                    9'007'199'254'740'992ULL;
                return invalid;
            },
            "lichess_bot"),
        std::runtime_error);
    REQUIRE_THROWS_AS(
        arena::application::ServiceCatalog::provider_status(
            root / "data",
            [](const fs::path&,
               const fs::path&,
               const std::vector<std::string>&,
               const std::chrono::milliseconds) {
                json invalid = provider_status_v3_fixture();
                invalid["runtime_updated_at"] = "2026-13-99T99:99:99Z";
                return invalid;
            },
            "lichess_bot"),
        std::runtime_error);
    bool launched = false;
    REQUIRE_THROWS_AS(
        arena::application::ServiceCatalog::provider_status(
            root / "data",
            [&launched](
                const fs::path&,
                const fs::path&,
                const std::vector<std::string>&,
                const std::chrono::milliseconds) {
                launched = true;
                return provider_status_v3_fixture();
            },
            "unknown"),
        std::invalid_argument);
    REQUIRE_FALSE(launched);
    fs::remove_all(root);
}

TEST_CASE("provider status consumes closed v4 and v5 without acquiring archive authority") {
    const fs::path root =
        fs::temp_directory_path() /
        ("arena-provider-status-current-" + arena::domain::uuid_v7());
    write_ready_bot_fixture(root, "node", true, true, "1.1", true, true);

    const json v4 = arena::application::ServiceCatalog::provider_status(
        root / "data",
        [](const fs::path&,
           const fs::path&,
           const std::vector<std::string>&,
           const std::chrono::milliseconds) {
            return provider_status_v4_fixture();
        },
        "lichess_bot");
    REQUIRE(v4.at("provider_status_schema") == "uci_arena_lichess_status_v4");
    REQUIRE(v4.at("supervision_availability") == "available");
    REQUIRE(v4.at("incident_availability") == "available");
    REQUIRE(v4.at("status").at("supervision").at("restart_pending") == true);
    REQUIRE(v4.at("status").at("incidents").at("active_count") == 2);

    const json v5 = arena::application::ServiceCatalog::provider_status(
        root / "data",
        [](const fs::path&,
           const fs::path&,
           const std::vector<std::string>&,
           const std::chrono::milliseconds) {
            return provider_status_v5_fixture();
        },
        "lichess_bot");
    REQUIRE(v5.at("provider_status_schema") == "uci_arena_lichess_status_v5");
    REQUIRE(v5.at("status").at("games").at("total") == 646);

    REQUIRE_THROWS_AS(
        arena::application::ServiceCatalog::provider_status(
            root / "data",
            [](const fs::path&,
               const fs::path&,
               const std::vector<std::string>&,
               const std::chrono::milliseconds) {
                json invalid = provider_status_v4_fixture();
                invalid["supervision"]["last_exit"]["command"] =
                    "MUST_NOT_ESCAPE";
                return invalid;
            },
            "lichess_bot"),
        std::runtime_error);
    REQUIRE_THROWS_AS(
        arena::application::ServiceCatalog::provider_status(
            root / "data",
            [](const fs::path&,
               const fs::path&,
               const std::vector<std::string>&,
               const std::chrono::milliseconds) {
                json invalid = provider_status_v5_fixture();
                invalid["games"]["storage_bytes"] =
                    9'007'199'254'740'992ULL;
                return invalid;
            },
            "lichess_bot"),
        std::runtime_error);
    fs::remove_all(root);
}

TEST_CASE("provider incidents use the declared operation and validate cursor pages") {
    const fs::path root =
        fs::temp_directory_path() /
        ("arena-provider-incidents-" + arena::domain::uuid_v7());
    write_ready_bot_fixture(root, "node", true, true, "1.1", true, true);
    fs::path observed_executable;
    fs::path observed_working_directory;
    std::vector<std::string> observed_arguments;
    const json result = arena::application::ServiceCatalog::provider_incidents(
        root / "data",
        [&observed_executable, &observed_working_directory, &observed_arguments](
            const fs::path& executable,
            const fs::path& working_directory,
            const std::vector<std::string>& arguments,
            const std::chrono::milliseconds timeout) {
            observed_executable = executable;
            observed_working_directory = working_directory;
            observed_arguments = arguments;
            REQUIRE(timeout == std::chrono::seconds(30));
            return provider_incident_page_fixture("all", 11);
        },
        "lichess_bot",
        "all",
        11,
        25);

    REQUIRE(result.at("schema") ==
            "arena_provider_incident_page_projection_v1");
    REQUIRE(result.at("availability") == "available");
    REQUIRE(result.at("provider_contract_version") == "1.1");
    REQUIRE(result.at("page").at("incidents").size() == 2);
    REQUIRE(result.at("page").at("next_cursor") == 19);
    REQUIRE(observed_executable.filename() == "adapter.mjs");
    REQUIRE(fs::is_directory(observed_working_directory));
    REQUIRE(observed_arguments.size() == 10);
    REQUIRE(observed_arguments.at(0) == "alerts");
    REQUIRE(observed_arguments.at(1) == "scan");
    REQUIRE(observed_arguments.at(2) == "--filter");
    REQUIRE(observed_arguments.at(3) == "all");
    REQUIRE(observed_arguments.at(4) == "--after-cursor");
    REQUIRE(observed_arguments.at(5) == "11");
    REQUIRE(observed_arguments.at(6) == "--limit");
    REQUIRE(observed_arguments.at(7) == "25");
    REQUIRE(observed_arguments.at(8) == "--runtime-dir");
    REQUIRE(fs::is_directory(observed_arguments.at(9)));

    const auto invalid_page = [&root](const json& page) {
        return arena::application::ServiceCatalog::provider_incidents(
            root / "data",
            [&page](const fs::path&,
                    const fs::path&,
                    const std::vector<std::string>&,
                    const std::chrono::milliseconds) { return page; },
            "lichess_bot",
            "active",
            0,
            100);
    };
    json non_monotonic = provider_incident_page_fixture();
    non_monotonic["incidents"][1]["cursor"] = 3;
    REQUIRE_THROWS_AS(invalid_page(non_monotonic), std::runtime_error);
    json private_detail = provider_incident_page_fixture();
    private_detail["incidents"][0]["detail"] =
        "credential at C:\\private\\provider";
    REQUIRE_THROWS_AS(invalid_page(private_detail), std::runtime_error);
    json extra_field = provider_incident_page_fixture();
    extra_field["incidents"][0]["raw_output"] = "MUST_NOT_ESCAPE";
    REQUIRE_THROWS_AS(invalid_page(extra_field), std::runtime_error);
    fs::remove_all(root);
}

TEST_CASE("provider incidents are explicitly unavailable when the operation is absent") {
    const fs::path root =
        fs::temp_directory_path() /
        ("arena-provider-incidents-legacy-" + arena::domain::uuid_v7());
    write_ready_bot_fixture(root);
    bool launched = false;
    const json result = arena::application::ServiceCatalog::provider_incidents(
        root / "data",
        [&launched](const fs::path&,
                    const fs::path&,
                    const std::vector<std::string>&,
                    const std::chrono::milliseconds) {
            launched = true;
            return provider_incident_page_fixture();
        },
        "lichess_bot",
        "active",
        0,
        100);
    REQUIRE(result.at("availability") == "unavailable");
    REQUIRE(result.at("page").is_null());
    REQUIRE_FALSE(launched);

    REQUIRE_THROWS_AS(
        arena::application::ServiceCatalog::provider_incidents(
            root / "data",
            [&launched](const fs::path&,
                        const fs::path&,
                        const std::vector<std::string>&,
                        const std::chrono::milliseconds) {
                launched = true;
                return provider_incident_page_fixture();
            },
            "lichess_bot",
            "private",
            0,
            100),
        std::invalid_argument);
    REQUIRE_FALSE(launched);
    fs::remove_all(root);
}

TEST_CASE("provider games use declared operations and stable descending cursors") {
    const fs::path root =
        fs::temp_directory_path() /
        ("arena-provider-games-" + arena::domain::uuid_v7());
    write_ready_bot_fixture(
        root, "node", true, true, "1.1", true, true, true);
    std::vector<std::vector<std::string>> calls;
    const auto runner = [&calls](
                            const fs::path& executable,
                            const fs::path& working_directory,
                            const std::vector<std::string>& arguments,
                            const std::chrono::milliseconds timeout) {
        REQUIRE(executable.filename() == "adapter.mjs");
        REQUIRE(fs::is_directory(working_directory));
        REQUIRE(timeout == std::chrono::seconds(30));
        calls.push_back(arguments);
        if (arguments.at(0) == "matches") {
            return provider_game_page_fixture("history", 11);
        }
        return provider_game_detail_fixture("fixture-game-nine");
    };

    const json page = arena::application::ServiceCatalog::provider_games(
        root / "data", runner, "lichess_bot", "history", 11, 25);
    REQUIRE(page.at("schema") == "arena_provider_game_page_projection_v1");
    REQUIRE(page.at("availability") == "available");
    REQUIRE(page.at("page").at("games").size() == 2);
    REQUIRE(page.at("page").at("next_cursor") == 7);
    REQUIRE(calls.at(0) == std::vector<std::string>{
        "matches", "scan", "--scope", "history", "--before", "11",
        "--limit", "25", "--runtime-dir", calls.at(0).at(9)});
    REQUIRE(fs::is_directory(calls.at(0).at(9)));

    const json detail = arena::application::ServiceCatalog::provider_game(
        root / "data", runner, "lichess_bot", "fixture-game-nine");
    REQUIRE(detail.at("schema") == "arena_provider_game_projection_v1");
    REQUIRE(detail.at("game").at("archive_completeness") == "complete");
    REQUIRE(detail.at("game").at("replay").size() == 2);
    REQUIRE(calls.at(1).at(0) == "match");
    REQUIRE(calls.at(1).at(1) == "inspect");
    REQUIRE(calls.at(1).at(2) == "--game-id");
    REQUIRE(calls.at(1).at(3) == "fixture-game-nine");
    REQUIRE(calls.at(1).at(4) == "--runtime-dir");
    REQUIRE(fs::is_directory(calls.at(1).at(5)));

    const auto invalid_page = [&root](const json& candidate) {
        return arena::application::ServiceCatalog::provider_games(
            root / "data",
            [&candidate](const fs::path&,
                         const fs::path&,
                         const std::vector<std::string>&,
                         const std::chrono::milliseconds) { return candidate; },
            "lichess_bot",
            "history",
            11,
            100);
    };
    json non_monotonic = provider_game_page_fixture("history", 11);
    non_monotonic["games"][1]["cursor"] = 10;
    REQUIRE_THROWS_AS(invalid_page(non_monotonic), std::runtime_error);
    json extra_field = provider_game_page_fixture("history", 11);
    extra_field["games"][0]["storage_path"] = "MUST_NOT_ESCAPE";
    REQUIRE_THROWS_AS(invalid_page(extra_field), std::runtime_error);
    json secret_looking = provider_game_page_fixture("history", 11);
    secret_looking["games"][0]["speed"] = "github_pat-aaaaaaaa";
    REQUIRE_THROWS_AS(invalid_page(secret_looking), std::runtime_error);

    json broken_replay = provider_game_detail_fixture();
    broken_replay["replay"][1]["before_fen"] = broken_replay["initial_fen"];
    REQUIRE_THROWS_AS(
        arena::application::ServiceCatalog::provider_game(
            root / "data",
            [&broken_replay](const fs::path&,
                             const fs::path&,
                             const std::vector<std::string>&,
                             const std::chrono::milliseconds) {
                return broken_replay;
            },
            "lichess_bot",
            "fixture-game-nine"),
        std::runtime_error);
    json path_bearing_replay = provider_game_detail_fixture();
    path_bearing_replay["replay"][0]["before_fen"] = "C:\\private\\archive";
    REQUIRE_THROWS_AS(
        arena::application::ServiceCatalog::provider_game(
            root / "data",
            [&path_bearing_replay](const fs::path&,
                                   const fs::path&,
                                   const std::vector<std::string>&,
                                   const std::chrono::milliseconds) {
                return path_bearing_replay;
            },
            "lichess_bot",
            "fixture-game-nine"),
        std::runtime_error);
    fs::remove_all(root);
}

TEST_CASE("provider games are explicitly unavailable when capability is absent") {
    const fs::path root =
        fs::temp_directory_path() /
        ("arena-provider-games-legacy-" + arena::domain::uuid_v7());
    write_ready_bot_fixture(root);
    bool launched = false;
    const auto runner = [&launched](
                            const fs::path&,
                            const fs::path&,
                            const std::vector<std::string>&,
                            const std::chrono::milliseconds) {
        launched = true;
        return provider_game_page_fixture();
    };
    const json page = arena::application::ServiceCatalog::provider_games(
        root / "data", runner, "lichess_bot", "history", 0, 100);
    REQUIRE(page.at("availability") == "unavailable");
    REQUIRE(page.at("page").is_null());
    const json detail = arena::application::ServiceCatalog::provider_game(
        root / "data", runner, "lichess_bot", "fixture-game-nine");
    REQUIRE(detail.at("availability") == "unavailable");
    REQUIRE(detail.at("game").is_null());
    REQUIRE_FALSE(launched);
    REQUIRE_THROWS_AS(
        arena::application::ServiceCatalog::provider_games(
            root / "data", runner, "lichess_bot", "private", 0, 100),
        std::invalid_argument);
    REQUIRE_THROWS_AS(
        arena::application::ServiceCatalog::provider_game(
            root / "data", runner, "lichess_bot", "../private"),
        std::invalid_argument);
    REQUIRE_FALSE(launched);
    fs::remove_all(root);
}

TEST_CASE("provider game keyset projection reaches 646 records exactly once") {
    const fs::path root =
        fs::temp_directory_path() /
        ("arena-provider-games-pagination-" + arena::domain::uuid_v7());
    write_ready_bot_fixture(
        root, "node", true, false, "1.1", true, true, true);
    const auto runner = [](
                            const fs::path&,
                            const fs::path&,
                            const std::vector<std::string>& arguments,
                            const std::chrono::milliseconds) {
        const auto before_flag =
            std::ranges::find(arguments, std::string{"--before"});
        REQUIRE(before_flag != arguments.end());
        const std::uint64_t before =
            std::stoull(*std::next(before_flag));
        std::uint64_t cursor = before == 0U ? 646U : before - 1U;
        json games = json::array();
        while (cursor > 0U && games.size() < 100U) {
            games.push_back(provider_game_summary_fixture(
                "history", cursor, "game-" + std::to_string(cursor),
                "summary_only"));
            --cursor;
        }
        const bool has_more = cursor > 0U;
        return json{
            {"schema", "uci_arena_provider_game_page_v1"},
            {"scope", "history"},
            {"before_cursor", before},
            {"next_cursor",
             games.empty() ? 0U
                           : games.back().at("cursor").get<std::uint64_t>()},
            {"has_more", has_more},
            {"games", std::move(games)},
        };
    };

    std::set<std::string> visited;
    std::uint64_t before = 0U;
    std::size_t pages = 0U;
    while (true) {
        const json projection =
            arena::application::ServiceCatalog::provider_games(
                root / "data",
                runner,
                "lichess_bot",
                "history",
                before,
                100);
        const json& page = projection.at("page");
        for (const auto& game : page.at("games")) {
            REQUIRE(visited.insert(game.at("game_id").get<std::string>()).second);
        }
        ++pages;
        if (!page.at("has_more").get<bool>()) {
            break;
        }
        before = page.at("next_cursor").get<std::uint64_t>();
    }
    REQUIRE(pages == 7U);
    REQUIRE(visited.size() == 646U);
    REQUIRE(visited.contains("game-1"));
    REQUIRE(visited.contains("game-646"));
    fs::remove_all(root);
}

TEST_CASE("Lichess BOT option updates are allowlisted and preserve explicit confirmations") {
    const fs::path root =
        fs::temp_directory_path() /
        ("arena-bot-options-update-" + arena::domain::uuid_v7());
    write_ready_bot_fixture(root);
    std::vector<std::vector<std::string>> calls;
    const auto runner =
        [&calls](
            const fs::path&,
            const fs::path&,
            const std::vector<std::string>& arguments,
            const std::chrono::milliseconds) {
            calls.push_back(arguments);
            json options = manager_bot_options_fixture();
            options["outbound_challenge"]["rated"] = true;
            return options;
        };
    const json settings{
        {"outbound_challenge", {{"rated", true}}},
        {"scheduler", {{"enabled", true}}},
    };
    const json result = arena::application::ServiceCatalog::update_bot_options(
        root / "data", runner, settings, true, true);
    REQUIRE(result.at("outbound_challenge").at("rated") == true);
    REQUIRE(calls.size() == 1);
    REQUIRE(calls.at(0).at(1) == "config");
    REQUIRE(calls.at(0).at(2) == "update");
    REQUIRE(std::find(calls.at(0).begin(), calls.at(0).end(),
                      "I_ACCEPT_RATED_BOT_GAMES") != calls.at(0).end());
    REQUIRE(std::find(calls.at(0).begin(), calls.at(0).end(),
                      "I_ACCEPT_AUTOMATIC_OUTBOUND_CHALLENGES") != calls.at(0).end());
    REQUIRE_THROWS_AS(
        arena::application::ServiceCatalog::update_bot_options(
            root / "data", runner, {{"engine", {{"executable", "x"}}}}, false, false),
        std::invalid_argument);
    REQUIRE(calls.size() == 1);
    fs::remove_all(root);
}

TEST_CASE("Lichess BOT local account actions are typed and sanitized") {
    const fs::path root =
        fs::temp_directory_path() /
        ("arena-bot-account-actions-" + arena::domain::uuid_v7());
    write_ready_bot_fixture(root);
    std::vector<std::vector<std::string>> calls;
    const auto runner =
        [&calls](
            const fs::path&,
            const fs::path&,
            const std::vector<std::string>& arguments,
            const std::chrono::milliseconds) {
            calls.push_back(arguments);
            if (arguments.at(1) == "owner" && arguments.at(2) == "list") {
                return json{{"status", "ready"},
                            {"owners", {{{"username", "ArenaOwner"}, {"created_at", 10}}}},
                            {"store", "MUST_NOT_ESCAPE"}};
            }
            if (arguments.at(1) == "owner") {
                const bool add = arguments.at(2) == "add";
                return json{{"status", "ready"}, {"username", "ArenaOwner"},
                            {add ? "added" : "removed", true}};
            }
            if (arguments.at(1) == "reserve") {
                return json{{"status", "ready"},
                            {"reservation",
                             {{"opponent", "OpponentBot"}, {"games_remaining", 3},
                              {"expires_at", 200}, {"created_at", 100}}}};
            }
            if (arguments.at(1) == "reservation" && arguments.at(2) == "status") {
                return json{{"status", "ready"}, {"reservation", nullptr}};
            }
            if (arguments.at(1) == "reservation") {
                return json{{"status", "ready"}, {"canceled", true}};
            }
            return json{{"status", "ready"},
                        {"scheduler", {{"paused", arguments.at(2) != "resume"}}}};
        };

    const json owner_result =
        arena::application::ServiceCatalog::control_bot_account(
            root / "data", runner, "owner_list", json::object());
    REQUIRE(owner_result.at("owners").size() == 1);
    REQUIRE(owner_result.dump().find("MUST_NOT_ESCAPE") == std::string::npos);
    REQUIRE(arena::application::ServiceCatalog::control_bot_account(
                root / "data", runner, "owner_add", {{"username", "ArenaOwner"}})
                .at("added") == true);
    REQUIRE(arena::application::ServiceCatalog::control_bot_account(
                root / "data", runner, "owner_remove", {{"username", "ArenaOwner"}})
                .at("removed") == true);
    REQUIRE(arena::application::ServiceCatalog::control_bot_account(
                root / "data", runner, "reservation_create",
                {{"opponent", "OpponentBot"}, {"games", 3}, {"minutes", 30}})
                .at("reservation").at("games_remaining") == 3);
    REQUIRE(arena::application::ServiceCatalog::control_bot_account(
                root / "data", runner, "reservation_status", json::object())
                .at("reservation").is_null());
    REQUIRE(arena::application::ServiceCatalog::control_bot_account(
                root / "data", runner, "reservation_cancel", json::object())
                .at("canceled") == true);
    for (const auto& action : {"scheduler_status", "scheduler_pause", "scheduler_resume"}) {
        REQUIRE(arena::application::ServiceCatalog::control_bot_account(
                    root / "data", runner, action, json::object())
                    .contains("scheduler"));
    }
    for (const auto& arguments : calls) {
        REQUIRE(arguments.at(arguments.size() - 2) == "--runtime-dir");
        REQUIRE(fs::is_directory(arguments.back()));
    }
    REQUIRE_THROWS_AS(
        arena::application::ServiceCatalog::control_bot_account(
            root / "data", runner, "owner_add", {{"username", "bad name"}}),
        std::invalid_argument);
    REQUIRE_THROWS_AS(
        arena::application::ServiceCatalog::control_bot_account(
            root / "data", runner, "owner_list", {{"command", "anything"}}),
        std::invalid_argument);
    REQUIRE_THROWS_AS(
        arena::application::ServiceCatalog::control_bot_account(
            root / "data", runner, "register", json::object()),
        std::invalid_argument);
    fs::remove_all(root);
}

TEST_CASE("Lichess BOT discovery rejects a credential inside the installed adapter") {
    const fs::path root =
        fs::temp_directory_path() /
        ("arena-bot-external-token-" + arena::domain::uuid_v7());
    write_ready_bot_fixture(root);
    const json adapter_receipt =
        read_json(root / "data" / "receipts" / "lichess.adapter.json");
    const fs::path adapter_root =
        adapter_receipt.at("active_root").get<std::string>();
    const fs::path legacy_token = adapter_root / "app" / "adapter.mjs";
    write_json(
        root / "data" / "locators" / "lichess_token_path.json",
        {
            {"schema", "arena_saved_locator_v1"},
            {"schema_version", 1},
            {"kind", "lichess_token_path"},
            {"mode", "in_place_reference"},
            {"path", legacy_token.string()},
            {"validation", json::object()},
        });

    const json discovery =
        arena::application::ServiceCatalog::discover_bot(root / "data");
    const json& candidate = discovery.at("candidates").at(0);
    REQUIRE(candidate.at("state") == "blocked");
    REQUIRE(candidate.at("token_reference_available") == false);
    REQUIRE(candidate.at("issues").dump().find(
                "bot_token_reference_not_external") != std::string::npos);
    fs::remove_all(root);
}

TEST_CASE("broker policy rejects unallowlisted services and actions") {
    const auto path =
        std::filesystem::temp_directory_path() /
        ("arena-broker-" + arena::domain::uuid_v7() + ".json");
    {
        std::ofstream output(path);
        output << R"({
          "schema_version": 1,
          "services": [{
            "id": "018f218e-7be1-7a56-9daa-3f003641c801",
            "platform_name": "ExampleService",
            "actions": ["status"]
          }]
        })";
    }
    const auto policy = arena::application::BrokerPolicy::load(path);
    REQUIRE(policy.size() == 1);
    REQUIRE(
        policy.authorize("018f218e-7be1-7a56-9daa-3f003641c801", "status").platform_name ==
        "ExampleService");
    REQUIRE_THROWS(
        policy.authorize("018f218e-7be1-7a56-9daa-3f003641c801", "restart"));
    REQUIRE_THROWS(
        policy.authorize("018f218e-7be1-7a56-9daa-3f003641c802", "status"));
    std::filesystem::remove(path);
}

#ifdef _WIN32
TEST_CASE("Windows entry points honor the installed fixed-scope marker") {
    const fs::path executable = arena::platform::windows_executable_path();
    REQUIRE_FALSE(executable.empty());
    const fs::path marker =
        executable.parent_path().parent_path() /
        "share" / "uci-arena" / "install-scope.txt";
    if (!fs::is_regular_file(marker)) {
        REQUIRE(
            arena::platform::windows_install_scope() ==
            arena::platform::WindowsInstallScope::per_machine);
        return;
    }
    std::ifstream input(marker, std::ios::binary);
    std::string expected;
    std::getline(input, expected);
    if (!expected.empty() && expected.back() == '\r') {
        expected.pop_back();
    }
    REQUIRE((expected == "per-user" || expected == "per-machine"));
    REQUIRE(
        arena::platform::windows_install_scope() ==
        (expected == "per-user"
             ? arena::platform::WindowsInstallScope::per_user
             : arena::platform::WindowsInstallScope::per_machine));
}
#endif
