#include "arena/application/engine_onboarding.hpp"
#include "arena/domain/uuid.hpp"

#include <catch2/catch_test_macros.hpp>
#include <sodium.h>

#include <array>
#include <filesystem>
#include <fstream>

namespace {

namespace fs = std::filesystem;
using nlohmann::json;

fs::path executable_path(const fs::path& root, const std::string& stem) {
#ifdef _WIN32
    return root / (stem + ".exe");
#else
    return root / stem;
#endif
}

void write_executable(const fs::path& path) {
    fs::create_directories(path.parent_path());
    std::ofstream(path, std::ios::binary) << "mock";
#ifndef _WIN32
    fs::permissions(
        path,
        fs::perms::owner_read | fs::perms::owner_write |
            fs::perms::owner_exec,
        fs::perm_options::replace);
#endif
}

std::string sha256_file(const fs::path& path) {
    REQUIRE(sodium_init() >= 0);
    std::ifstream input(path, std::ios::binary);
    REQUIRE(input.good());
    crypto_hash_sha256_state state{};
    crypto_hash_sha256_init(&state);
    std::array<unsigned char, 4096> buffer{};
    while (input) {
        input.read(reinterpret_cast<char*>(buffer.data()), buffer.size());
        const auto count = input.gcount();
        if (count > 0) {
            crypto_hash_sha256_update(
                &state, buffer.data(),
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

json file_inventory_entry(
    const fs::path& active_root,
    const fs::path& file) {
    return {
        {"path", fs::relative(file, active_root).generic_string()},
        {"sha256", sha256_file(file)},
        {"size_bytes", fs::file_size(file)},
    };
}

json uci_discovery(const std::string& entrypoint) {
    return json::array({
        {
            {"kind", "uci_engine"},
            {"locator",
             {{"source", "entrypoint"}, {"entrypoint", entrypoint}}},
        },
    });
}

json managed_launch_profile(
    const fs::path& active_root,
    const fs::path& executable,
    const std::string& component_id,
    const std::string& component_version) {
    return {
        {"schema", "arena_uci_engine_launch_profile_v1"},
        {"schema_version", 1},
        {"component",
         {{"id", component_id},
          {"version", component_version},
          {"root", fs::weakly_canonical(active_root).string()}}},
        {"engine",
         {{"executable", fs::weakly_canonical(executable).string()},
          {"working_directory", fs::weakly_canonical(active_root).string()},
          {"arguments", json::array()}}},
        {"state", "qualified_timing"},
        {"enabled", true},
        {"uci_options",
         {{"ModelRoot", (active_root / "models" / "default").string()},
          {"SearchCore", "mcgs"},
          {"OnnxProvider", "directml"},
          {"Threads", 3},
          {"OwnBook", true}}},
        {"expected_runtime",
         {{"timing_contract_version",
           "persistent_mcgs_relative_clock_temporal_v4"},
          {"model_id", "example_model_v1"},
          {"model_fingerprint", "8104116015256418347"},
          {"adapter_id", "example_adapter_v1"},
          {"provider_requested", "directml"},
          {"provider_effective", "directml"},
          {"provider_numeric_fingerprint", "3628410929205560055"},
          {"hardware_driver_fingerprint", "15113055170282736143"},
          {"onnx_runtime_version", "1.24.4"},
          {"search_core", "mcgs"}}},
        {"evidence",
         {{"timing_profile",
           {{"schema", "uci_time_profile_v3"},
            {"identity_sha256", "sha256:fixture"}}}}},
        {"knowledge", {{"opening_book", {{"status", "selected"}}}}},
        {"diagnostics", json::array()},
    };
}

}  // namespace

TEST_CASE("installed engine receipts resolve provider-declared UCI entrypoints") {
    const fs::path root =
        fs::temp_directory_path() /
        ("arena-engine-onboarding-" + arena::domain::uuid_v7());
    const fs::path data_root = root / "data";
    const fs::path active_root = root / "components" / "engine";
    const fs::path executable =
        executable_path(active_root / "bin", "renamed-provider-engine");
    const fs::path workspace = root / "workspace" / "example-engine";
    const fs::path unrelated_configuration = workspace / "provider.json";
    write_executable(executable);
    fs::create_directories(data_root / "receipts");
    fs::create_directories(workspace);
    std::ofstream(unrelated_configuration)
        << json{{"schema", "example_private_configuration_v1"}}.dump(2);

    std::ofstream(active_root / "arena-component.json")
        << json{
               {"schema", "arena_install_component_v1"},
               {"schema_version", 1},
               {"component_id", "example.engine"},
               {"component_version", "1.0.0"},
               {"entrypoints",
                {{"uci",
                  fs::relative(executable, active_root).generic_string()}}},
               {"discovery", uci_discovery("uci")},
               {"files", {file_inventory_entry(active_root, executable)}},
           }
               .dump(2);
    std::ofstream(data_root / "receipts" / "example.engine.json")
        << json{
               {"schema", "arena_install_receipt_v1"},
               {"schema_version", 1},
               {"component_id", "example.engine"},
               {"component_version", "1.0.0"},
               {"state", "installed_managed"},
               {"active_root", active_root.string()},
               {"artifact_sha256", std::string(64, 'a')},
               {"data_paths", {workspace.string()}},
               {"configuration_path", unrelated_configuration.string()},
           }
               .dump(2);

    const json discovered =
        arena::application::EngineOnboarding::discover(
            data_root, {}, false);
    REQUIRE(discovered.at("candidates").size() == 1);
    const json& candidate = discovered.at("candidates").at(0);
    REQUIRE(candidate.at("source") == "install_receipt");
    REQUIRE(candidate.at("authority") == "verified_component_inventory");
    REQUIRE(candidate.at("state") == "ready_to_probe");
    REQUIRE_FALSE(candidate.contains("launch_profile"));
    REQUIRE(candidate.at("component_id") == "example.engine");
    REQUIRE(
        fs::path(candidate.at("executable").get<std::string>()) ==
        fs::weakly_canonical(executable));
    REQUIRE(
        fs::path(candidate.at("working_directory").get<std::string>()) ==
        fs::weakly_canonical(active_root));

    fs::remove_all(root);
}

TEST_CASE("managed discovery role resolves a differently named entrypoint") {
    const fs::path root =
        fs::temp_directory_path() /
        ("arena-stockfish-onboarding-" + arena::domain::uuid_v7());
    const fs::path data_root = root / "data";
    const fs::path active_root = root / "components" / "stockfish.engine" / "15.1";
    const fs::path executable = executable_path(active_root / "bin", "stockfish");
    write_executable(executable);
    fs::create_directories(data_root / "receipts");

    std::ofstream(active_root / "arena-component.json")
        << json{
               {"schema", "arena_install_component_v1"},
               {"schema_version", 1},
               {"component_id", "stockfish.engine"},
               {"component_version", "15.1"},
               {"entrypoints",
                {{"executable",
                  fs::relative(executable, active_root).generic_string()}}},
               {"discovery", uci_discovery("executable")},
               {"files", {file_inventory_entry(active_root, executable)}},
           }
               .dump(2);
    std::ofstream(data_root / "receipts" / "stockfish.engine.json")
        << json{
               {"schema", "arena_install_receipt_v1"},
               {"schema_version", 1},
               {"component_id", "stockfish.engine"},
               {"component_version", "15.1"},
               {"state", "installed_managed"},
               {"active_root", active_root.string()},
               {"artifact_sha256", std::string(64, 'b')},
           }
               .dump(2);

    const json discovered =
        arena::application::EngineOnboarding::discover(data_root, {}, false);
    REQUIRE(discovered.at("candidates").size() == 1);
    const json& candidate = discovered.at("candidates").at(0);
    REQUIRE(candidate.at("source") == "install_receipt");
    REQUIRE(candidate.at("component_id") == "stockfish.engine");
    REQUIRE(
        fs::path(candidate.at("executable").get<std::string>()) ==
        fs::weakly_canonical(executable));

    fs::remove_all(root);
}

TEST_CASE("verified UCI Arena receipt is recommended without turning its model into the product") {
    const fs::path root =
        fs::temp_directory_path() /
        ("arena-engine-recommended-" + arena::domain::uuid_v7());
    const fs::path data_root = root / "data";
    const fs::path active_root = root / "components" / "uci_arena.engine" / "1.2.3";
    const fs::path executable = executable_path(active_root / "bin", "uci-arena");
    write_executable(executable);
    const fs::path model_root = active_root / "models" / "default";
    fs::create_directories(model_root);
    fs::create_directories(data_root / "receipts");
    std::ofstream(active_root / "arena-component.json")
        << json{
               {"schema", "arena_install_component_v1"},
               {"schema_version", 1},
               {"component_id", "uci_arena.engine"},
               {"component_version", "1.2.3"},
               {"display_name", "UCI Arena Engine"},
               {"entrypoints", {{"uci", fs::relative(executable, active_root).generic_string()}}},
               {"discovery", uci_discovery("uci")},
               {"default_model",
                {{"model_id", "example_model_v1"},
                 {"display_name", "Example Model"},
                 {"input_adapter_id", "example_dense_planes_v1"},
                 {"root", "models/default"}}},
               {"files", {file_inventory_entry(active_root, executable)}},
           }
               .dump(2);
    std::ofstream(data_root / "receipts" / "uci_arena.engine.json")
        << json{
               {"schema", "arena_install_receipt_v1"},
               {"schema_version", 1},
               {"component_id", "uci_arena.engine"},
               {"component_version", "1.2.3"},
               {"state", "installed_managed"},
               {"active_root", active_root.string()},
               {"artifact_sha256", std::string(64, 'a')},
           }
               .dump(2);

    const json discovered = arena::application::EngineOnboarding::discover(data_root, {}, false);
    REQUIRE(discovered.at("candidates").size() == 1);
    const json& candidate = discovered.at("candidates").at(0);
    REQUIRE(candidate.at("recommended") == true);
    REQUIRE(candidate.at("display_name") == "UCI Arena Engine");
    REQUIRE(candidate.at("default_model").at("display_name") == "Example Model");
    REQUIRE(candidate.at("display_name") != candidate.at("default_model").at("display_name"));
    REQUIRE(
        fs::path(candidate.at("uci_options").at("ModelRoot").get<std::string>()) ==
        fs::weakly_canonical(model_root));
    fs::remove_all(root);
}

TEST_CASE("managed engine receipt projects its Engine-owned launch profile") {
    const fs::path root =
        fs::temp_directory_path() /
        ("arena-engine-launch-profile-" + arena::domain::uuid_v7());
    const fs::path data_root = root / "data";
    const fs::path active_root = root / "components" / "example.engine" / "1.2.3";
    const fs::path executable = executable_path(active_root / "bin", "renamed-engine");
    const fs::path workspace = root / "workspace" / "example-engine";
    const fs::path configuration = workspace / "manager-config.json";
    write_executable(executable);
    fs::create_directories(active_root / "models" / "default");
    fs::create_directories(data_root / "receipts");
    fs::create_directories(workspace);
    std::ofstream(active_root / "arena-component.json")
        << json{
               {"schema", "arena_install_component_v1"},
               {"schema_version", 1},
               {"component_id", "example.engine"},
               {"component_version", "1.2.3"},
               {"entrypoints",
                {{"uci", fs::relative(executable, active_root).generic_string()}}},
               {"discovery", uci_discovery("uci")},
               {"capabilities", {"uci_engine_launch_profile_v1"}},
               {"default_model",
                {{"model_id", "example_model_v1"},
                 {"display_name", "Example Model"},
                 {"input_adapter_id", "example_adapter_v1"},
                 {"root", "models/default"}}},
               {"files", {file_inventory_entry(active_root, executable)}},
           }
               .dump(2);
    std::ofstream(configuration)
        << managed_launch_profile(
               active_root, executable, "example.engine", "1.2.3")
               .dump(2);
    std::ofstream(data_root / "receipts" / "example.engine.json")
        << json{
               {"schema", "arena_install_receipt_v1"},
               {"schema_version", 1},
               {"component_id", "example.engine"},
               {"component_version", "1.2.3"},
               {"state", "installed_managed"},
               {"active_root", active_root.string()},
               {"artifact_sha256", std::string(64, 'a')},
               {"data_paths", {workspace.string()}},
               {"configuration_path", configuration.string()},
           }
               .dump(2);

    const json discovered =
        arena::application::EngineOnboarding::discover(data_root, {}, false);
    REQUIRE(discovered.at("candidates").size() == 1);
    const json& candidate = discovered.at("candidates").at(0);
    REQUIRE(candidate.at("state") == "ready_to_probe");
    REQUIRE(candidate.at("uci_options").at("SearchCore") == "mcgs");
    REQUIRE(candidate.at("uci_options").at("OnnxProvider") == "directml");
    REQUIRE(candidate.at("uci_options").at("Threads") == 3);
    REQUIRE(candidate.at("launch_profile").at("state") == "qualified_timing");
    REQUIRE(candidate.at("launch_profile").at("timing_profile").at("available") == true);

    json additive = managed_launch_profile(
        active_root, executable, "example.engine", "1.2.3");
    additive["producer_metadata"] = {{"capability", "future_addition"}};
    additive["component"]["release_channel"] = "development";
    additive["engine"]["environment"] = json::object();
    std::ofstream(configuration, std::ios::trunc) << additive.dump(2);
    const json compatible =
        arena::application::EngineOnboarding::discover(data_root, {}, false);
    REQUIRE(compatible.at("candidates").size() == 1);
    REQUIRE(compatible.at("candidates").at(0).at("state") == "ready_to_probe");

    json stale = managed_launch_profile(
        active_root, executable, "example.engine", "1.2.2");
    std::ofstream(configuration, std::ios::trunc) << stale.dump(2);
    const json rejected =
        arena::application::EngineOnboarding::discover(data_root, {}, false);
    REQUIRE(rejected.at("candidates").size() == 1);
    REQUIRE(rejected.at("candidates").at(0).at("state") == "unavailable");
    REQUIRE(
        rejected.at("candidates").at(0).at("issues").at(0).at("code") ==
        "engine_launch_profile_invalid");

    fs::remove_all(root);
}

TEST_CASE("managed components do not gain an engine role from names or capabilities") {
    const fs::path root =
        fs::temp_directory_path() /
        ("arena-engine-no-role-" + arena::domain::uuid_v7());
    const fs::path data_root = root / "data";
    const fs::path active_root = root / "components" / "uci_arena.engine";
    const fs::path executable =
        executable_path(active_root / "bin", "uci_arena_engine");
    write_executable(executable);
    fs::create_directories(data_root / "receipts");

    std::ofstream(active_root / "arena-component.json")
        << json{
               {"schema", "arena_install_component_v1"},
               {"schema_version", 1},
               {"component_id", "uci_arena.engine"},
               {"component_version", "1.0.0"},
               {"entrypoints",
                {{"uci", fs::relative(executable, active_root).generic_string()}}},
               {"capabilities", {"uci_engine"}},
               {"files", {file_inventory_entry(active_root, executable)}},
           }
               .dump(2);
    std::ofstream(data_root / "receipts" / "uci_arena.engine.json")
        << json{
               {"schema", "arena_install_receipt_v1"},
               {"schema_version", 1},
               {"component_id", "uci_arena.engine"},
               {"component_version", "1.0.0"},
               {"state", "installed_managed"},
               {"active_root", active_root.string()},
               {"artifact_sha256", std::string(64, 'a')},
           }
               .dump(2);

    const json discovered =
        arena::application::EngineOnboarding::discover(data_root, {}, false);
    REQUIRE(discovered.at("candidates").empty());
    fs::remove_all(root);
}

TEST_CASE("managed engine discovery rejects a modified executable") {
    const fs::path root =
        fs::temp_directory_path() /
        ("arena-engine-modified-" + arena::domain::uuid_v7());
    const fs::path data_root = root / "data";
    const fs::path active_root = root / "components" / "replacement.engine";
    const fs::path executable =
        executable_path(active_root / "bin", "replacement");
    write_executable(executable);
    const json original_file = file_inventory_entry(active_root, executable);
    fs::create_directories(data_root / "receipts");
    std::ofstream(active_root / "arena-component.json")
        << json{
               {"schema", "arena_install_component_v1"},
               {"schema_version", 1},
               {"component_id", "replacement.engine"},
               {"component_version", "2.0.0"},
               {"entrypoints",
                {{"runtime", fs::relative(executable, active_root).generic_string()}}},
               {"discovery", uci_discovery("runtime")},
               {"files", {original_file}},
           }
               .dump(2);
    std::ofstream(executable, std::ios::binary | std::ios::app) << "tampered";
    std::ofstream(data_root / "receipts" / "replacement.engine.json")
        << json{
               {"schema", "arena_install_receipt_v1"},
               {"schema_version", 1},
               {"component_id", "replacement.engine"},
               {"component_version", "2.0.0"},
               {"state", "installed_managed"},
               {"active_root", active_root.string()},
               {"artifact_sha256", std::string(64, 'c')},
           }
               .dump(2);

    const json discovered =
        arena::application::EngineOnboarding::discover(data_root, {}, false);
    REQUIRE(discovered.at("candidates").empty());
    fs::remove_all(root);
}

TEST_CASE("explicit bounded roots discover renamed engine executables") {
    const fs::path root =
        fs::temp_directory_path() /
        ("arena-engine-root-" + arena::domain::uuid_v7());
    const fs::path data_root = root / "data";
    const fs::path executable =
        executable_path(root / "selected" / "nested", "anything-user-chose");
    write_executable(executable);

    const json discovered =
        arena::application::EngineOnboarding::discover(
            data_root, {root / "selected"}, false);
    REQUIRE(discovered.at("candidates").size() == 1);
    const json& candidate = discovered.at("candidates").at(0);
    REQUIRE(candidate.at("source") == "discovered");
    REQUIRE(candidate.at("authority") == "discovery_hint");
    REQUIRE(candidate.at("state") == "ready_to_probe");
    REQUIRE(candidate.at("observation").at("path_kind") == "regular_file");
    REQUIRE(
        !candidate.at("observation").at("object_identity")
             .get<std::string>()
             .empty());
    REQUIRE(discovered.at("explicit_root_inspection").at("complete") == true);
    const fs::path candidate_executable =
        candidate.at("executable").get<std::string>();
    REQUIRE(candidate_executable.is_absolute());
    REQUIRE(fs::equivalent(candidate_executable, executable));

    fs::remove_all(root);
}

TEST_CASE("an explicitly selected executable is inspected without traversal") {
    const fs::path root =
        fs::temp_directory_path() /
        ("arena-engine-selected-file-" + arena::domain::uuid_v7());
    const fs::path executable = executable_path(root, "selected-engine");
    write_executable(executable);

    const json discovered = arena::application::EngineOnboarding::discover(
        root / "data", {executable}, false);
    REQUIRE(discovered.at("candidates").size() == 1);
    REQUIRE(
        fs::path(
            discovered.at("candidates").at(0).at("executable")
                .get<std::string>()) ==
        fs::absolute(executable).lexically_normal());
    REQUIRE(
        discovered.at("explicit_root_inspection").at("work").at("entries") ==
        0);
    REQUIRE(
        discovered.at("explicit_root_inspection")
                .at("work")
                .at("metadata_calls") == 1);

    fs::remove_all(root);
}

TEST_CASE("explicit-root entry budget counts rejected entries and fails closed") {
    const fs::path root =
        fs::temp_directory_path() /
        ("arena-engine-root-budget-" + arena::domain::uuid_v7());
    const fs::path selected = root / "selected";
    fs::create_directories(selected / "directory-a");
    fs::create_directories(selected / "directory-b");
    fs::create_directories(selected / "directory-c");
    write_executable(executable_path(selected, "would-be-candidate"));

    arena::application::EngineDiscoveryOptions options;
    options.max_total_entries = 2;
    const json discovered = arena::application::EngineOnboarding::discover(
        root / "data", {selected}, false, options);
    const json& inspection = discovered.at("explicit_root_inspection");
    REQUIRE(inspection.at("complete") == false);
    REQUIRE(inspection.at("work").at("entries") == 3);
    REQUIRE(discovered.at("candidates").empty());
    REQUIRE(
        inspection.at("diagnostics").at(0).at("code") ==
        "entry_budget_exceeded");

    fs::remove_all(root);
}

TEST_CASE("explicit-root cancellation prevents filesystem traversal") {
    const fs::path root =
        fs::temp_directory_path() /
        ("arena-engine-root-cancel-" + arena::domain::uuid_v7());
    const fs::path selected = root / "selected";
    write_executable(executable_path(selected, "candidate"));

    std::stop_source cancellation;
    cancellation.request_stop();
    arena::application::EngineDiscoveryOptions options;
    options.cancellation = cancellation.get_token();
    const json discovered = arena::application::EngineOnboarding::discover(
        root / "data", {selected}, false, options);
    const json& inspection = discovered.at("explicit_root_inspection");
    REQUIRE(inspection.at("complete") == false);
    REQUIRE(inspection.at("cancelled") == true);
    REQUIRE(inspection.at("work").at("entries") == 0);
    REQUIRE(inspection.at("work").at("metadata_calls") == 0);
    REQUIRE(discovered.at("candidates").empty());

    fs::remove_all(root);
}

TEST_CASE("explicit-root deadline exhaustion prevents filesystem traversal") {
    const fs::path root =
        fs::temp_directory_path() /
        ("arena-engine-root-deadline-" + arena::domain::uuid_v7());
    const fs::path selected = root / "selected";
    write_executable(executable_path(selected, "candidate"));

    arena::application::EngineDiscoveryOptions options;
    options.max_elapsed = std::chrono::milliseconds{0};
    const json discovered = arena::application::EngineOnboarding::discover(
        root / "data", {selected}, false, options);
    const json& inspection = discovered.at("explicit_root_inspection");
    REQUIRE(inspection.at("complete") == false);
    REQUIRE(inspection.at("cancelled") == false);
    REQUIRE(inspection.at("work").at("entries") == 0);
    REQUIRE(inspection.at("work").at("metadata_calls") == 0);
    REQUIRE(discovered.at("candidates").empty());
    REQUIRE(
        inspection.at("diagnostics").at(0).at("code") ==
        "discovery_deadline_exceeded");

    fs::remove_all(root);
}

TEST_CASE("explicit-root link targets are never executable hints") {
    const fs::path root =
        fs::temp_directory_path() /
        ("arena-engine-root-link-" + arena::domain::uuid_v7());
    const fs::path external = root / "external";
    const fs::path executable = executable_path(external, "external-engine");
    write_executable(executable);
    const fs::path selected = root / "selected";
    fs::create_directories(selected);
    const fs::path link = executable_path(selected, "linked-engine");
    std::error_code error;
    fs::create_symlink(executable, link, error);
    if (error) {
        fs::remove_all(root);
        SKIP("platform could not create a symbolic link: " << error.message());
    }
    const json directory_discovery =
        arena::application::EngineOnboarding::discover(
            root / "data", {selected}, false);
    REQUIRE(directory_discovery.at("candidates").empty());
    REQUIRE(
        directory_discovery.at("explicit_root_inspection").at("complete") ==
        true);

    const json direct_discovery =
        arena::application::EngineOnboarding::discover(
            root / "data", {link}, false);
    REQUIRE(direct_discovery.at("candidates").empty());
    REQUIRE(
        direct_discovery.at("explicit_root_inspection").at("complete") ==
        true);
    fs::remove_all(root);
}
