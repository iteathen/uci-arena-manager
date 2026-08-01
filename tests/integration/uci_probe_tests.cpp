#include "arena/adapters/uci_process.hpp"
#include "arena/adapters/sqlite_store.hpp"
#include "arena/application/control_plane.hpp"
#include "arena/application/engine_onboarding.hpp"
#include "arena/domain/uuid.hpp"

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <memory>

#ifndef ARENA_MOCK_UCI_PATH
#error ARENA_MOCK_UCI_PATH must name the mock UCI engine
#endif

TEST_CASE("UCI probe discovers identity options and a legal bestmove") {
    const std::filesystem::path executable = ARENA_MOCK_UCI_PATH;
    const auto result = arena::adapters::UciProcess::probe(
        executable, executable.parent_path(), {}, std::chrono::seconds(5));
    REQUIRE(result.ok);
    REQUIRE(result.engine_name == "Arena Mock Engine");
    REQUIRE(result.engine_author == "UCI Arena Manager");
    REQUIRE(result.bestmove == "e2e4");
    REQUIRE(result.options.size() == 2);
    REQUIRE(result.options.front().name == "Threads");
    REQUIRE(result.options.front().type == "spin");
    REQUIRE(result.options.front().minimum == 1);
    REQUIRE(result.options.front().maximum == 16);
    REQUIRE(result.fingerprint.size() == 64);
    REQUIRE(result.runtime_model_fingerprint == "8104116015256418347");
    REQUIRE(result.runtime_provider == "cpu");
    REQUIRE(result.runtime_provider_fingerprint == "5805769137532863231");
}

TEST_CASE("engine registration derives preferred metadata from the verified receipt") {
    namespace fs = std::filesystem;
    using nlohmann::json;

    const fs::path root =
        fs::temp_directory_path() /
        ("arena-engine-registration-" + arena::domain::uuid_v7());
    const fs::path data_root = root / "data";
    const fs::path active_root =
        root / "components" / "uci_arena.engine" / "1.2.3";
#ifdef _WIN32
    const fs::path executable =
        active_root / "bin" / "uci_arena_engine.exe";
#else
    const fs::path executable =
        active_root / "bin" / "uci_arena_engine";
#endif
    fs::create_directories(executable.parent_path());
    fs::create_directories(data_root / "receipts");
    fs::copy_file(
        ARENA_MOCK_UCI_PATH,
        executable,
        fs::copy_options::overwrite_existing);

    std::ofstream(active_root / "arena-component.json")
        << json{
               {"schema", "arena_install_component_v1"},
               {"schema_version", 1},
               {"component_id", "uci_arena.engine"},
               {"component_version", "1.2.3"},
               {"display_name", "UCI Arena Engine"},
               {"entrypoints",
                {{"uci",
                  fs::relative(executable, active_root).generic_string()}}},
               {"default_model",
                {{"model_id", "example_model_v1"},
                 {"display_name", "Example Model"},
                 {"input_adapter_id", "dense_planes_v1"}}},
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

    const json discovery =
        arena::application::EngineOnboarding::discover(
            data_root, {}, false);
    REQUIRE(discovery.at("candidates").size() == 1);
    const std::string candidate_id =
        discovery.at("candidates").at(0).at("candidate_id");

    {
        auto store =
            std::make_shared<arena::adapters::SqliteStore>(
                root / "arena.db");
        arena::application::ControlPlane control_plane(store);
        control_plane.initialize();
        control_plane.reconcile_installed_services(data_root);

        const auto registration =
            [&](const std::string& selected_candidate_id) {
                return control_plane.submit(
                {
                    {"schema_version", 1},
                    {"request_id", arena::domain::uuid_v7()},
                    {"idempotency_key", arena::domain::uuid_v7()},
                    {"target",
                     {{"kind", "engine"}, {"id", nullptr}}},
                    {"action", "engine.register"},
                    {"expected_revision", nullptr},
                    {"dry_run", false},
                    {"parameters",
                     {
                         {"display_name", "UCI Arena Engine"},
                         {"executable", executable.string()},
                         {"working_directory",
                          executable.parent_path().string()},
                         {"arguments", json::array()},
                         {"candidate_id",
                          selected_candidate_id},
                         {"component_id", "forged.engine"},
                         {"component_version", "99.0.0"},
                         {"recommended", false},
                         {"default_model",
                          {{"model_id", "forged"},
                           {"display_name", "Forged"},
                           {"input_adapter_id", "forged"}}},
                     }},
                    },
                    "test");
            };

        const json verified =
            registration(candidate_id).at("resource").at("data");
        REQUIRE(verified.at("candidate_id") == candidate_id);
        REQUIRE(
            verified.at("onboarding_source") ==
            "install_receipt");
        REQUIRE(
            verified.at("component_id") == "uci_arena.engine");
        REQUIRE(verified.at("component_version") == "1.2.3");
        REQUIRE(verified.at("recommended") == true);
        REQUIRE(
            verified.at("default_model").at("display_name") ==
            "Example Model");

        const json forged =
            registration("engcand-forged")
                .at("resource")
                .at("data");
        REQUIRE_FALSE(forged.contains("candidate_id"));
        REQUIRE_FALSE(forged.contains("component_id"));
        REQUIRE_FALSE(forged.contains("component_version"));
        REQUIRE_FALSE(forged.contains("recommended"));
        REQUIRE_FALSE(forged.contains("default_model"));
    }

    fs::remove_all(root);
}
