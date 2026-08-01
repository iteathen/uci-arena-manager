#include "arena/application/engine_onboarding.hpp"
#include "arena/domain/uuid.hpp"

#include <catch2/catch_test_macros.hpp>

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

}  // namespace

TEST_CASE("installed engine receipts resolve provider-declared UCI entrypoints") {
    const fs::path root =
        fs::temp_directory_path() /
        ("arena-engine-onboarding-" + arena::domain::uuid_v7());
    const fs::path data_root = root / "data";
    const fs::path active_root = root / "components" / "engine";
    const fs::path executable =
        executable_path(active_root / "bin", "renamed-provider-engine");
    write_executable(executable);
    fs::create_directories(data_root / "receipts");

    std::ofstream(active_root / "arena-component.json")
        << json{
               {"schema", "arena_install_component_v1"},
               {"schema_version", 1},
               {"component_id", "example.engine"},
               {"component_version", "1.0.0"},
               {"entrypoints",
                {{"uci",
                  fs::relative(executable, active_root).generic_string()}}},
               {"files", json::array()},
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
           }
               .dump(2);

    const json discovered =
        arena::application::EngineOnboarding::discover(
            data_root, {}, false);
    REQUIRE(discovered.at("candidates").size() == 1);
    const json& candidate = discovered.at("candidates").at(0);
    REQUIRE(candidate.at("source") == "install_receipt");
    REQUIRE(candidate.at("state") == "ready_to_probe");
    REQUIRE(candidate.at("component_id") == "example.engine");
    REQUIRE(
        fs::path(candidate.at("executable").get<std::string>()) ==
        fs::weakly_canonical(executable));
    REQUIRE(
        fs::path(candidate.at("working_directory").get<std::string>()) ==
        fs::weakly_canonical(active_root));

    fs::remove_all(root);
}

TEST_CASE("managed UCI-engine capability resolves an executable entrypoint") {
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
               {"capabilities", {"uci_engine"}},
               {"files", json::array()},
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
    fs::create_directories(data_root / "receipts");
    std::ofstream(active_root / "arena-component.json")
        << json{
               {"schema", "arena_install_component_v1"},
               {"schema_version", 1},
               {"component_id", "uci_arena.engine"},
               {"component_version", "1.2.3"},
               {"display_name", "UCI Arena Engine"},
               {"entrypoints", {{"uci", fs::relative(executable, active_root).generic_string()}}},
               {"default_model",
                {{"model_id", "example_model_v1"},
                 {"display_name", "Example Model"},
                 {"input_adapter_id", "example_dense_planes_v1"}}},
               {"files", json::array()},
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
    REQUIRE(candidate.at("state") == "ready_to_probe");
    REQUIRE(
        fs::path(candidate.at("executable").get<std::string>()) ==
        fs::weakly_canonical(executable));

    fs::remove_all(root);
}
