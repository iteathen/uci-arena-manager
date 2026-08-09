#include "arena/adapters/sqlite_store.hpp"
#include "arena/application/broker_policy.hpp"
#include "arena/application/control_plane.hpp"
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
           "acquisition_retry_auth"}},
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
          {"challenge_timeout_seconds", 25}}},
        {"opponent_selection",
         {{"max_games_per_opponent_per_day", 1},
          {"timeout_cooldown_minutes", 30},
          {"decline_cooldown_minutes", 360},
          {"rating_target", 2200},
          {"rating_spread", 600}}},
    };
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
    const std::string& contract_version = "1.0") {
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

    const json operations{
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
          {"startup_uninstall",
           {{"arguments", json::array({"startup", "uninstall"})},
            {"mutates", true}, {"credential", "forbidden"}}}}},
    };
    const json service_contract{
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
    const std::vector<std::pair<std::string, json>> documents{
        {"contracts/service-contract.json", service_contract},
        {"contracts/manager-operations.json", operations},
        {"contracts/configuration-schema.json", json{{"schema", "fixture"}}},
        {"contracts/configuration-update-schema.json", json{{"schema", "fixture"}}},
        {"contracts/status-schema.json", json{{"schema", "fixture"}}},
    };
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
    const std::string& contract_version = "1.0") {
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
        contract_version);

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
        arena::application::ServiceCatalog::profiles().at(1).at("capabilities"));
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

    const json candidate =
        arena::application::ServiceCatalog::discover_bot(data)
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
        arena::application::ServiceCatalog::discover_bot(root / "data")
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
    REQUIRE(calls.size() == 3);
    REQUIRE(calls.at(0).at(1) == "probe");
    REQUIRE(calls.at(1).at(1) == "startup");
    REQUIRE(calls.at(1).at(2) == "install");
    REQUIRE(calls.at(2).at(1) == "status");
    REQUIRE(
        std::find(
            calls.at(1).begin(),
            calls.at(1).end(),
            "VerifiedArenaBot") != calls.at(1).end());
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
