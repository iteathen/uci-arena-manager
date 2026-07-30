#include <curl/curl.h>

#include <CLI/CLI.hpp>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <nlohmann/json.hpp>
#include <set>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

#include "arena/application/broker_policy.hpp"
#include "arena/application/installer/setup.hpp"

namespace {

namespace fs = std::filesystem;
using arena::application::installer::SetupEngine;
using arena::application::installer::SetupPaths;
using nlohmann::json;

const char* manager_service_name() {
#ifdef _WIN32
    return "UCIArenaManager";
#else
    return "uci-arena-manager.service";
#endif
}

bool manager_service_running() {
    const int status = arena::application::perform_service_action(
        manager_service_name(), "status");
#ifdef _WIN32
    return status == 4;  // SERVICE_RUNNING
#else
    return status == 0;
#endif
}

fs::path environment_path(const char* name, const fs::path& fallback) {
    if (const char* value = std::getenv(name); value != nullptr && *value != '\0') {
        return value;
    }
    return fallback;
}

SetupPaths default_paths() {
#ifdef _WIN32
    const fs::path program_files = environment_path("ProgramFiles", "C:\\Program Files");
    const fs::path program_data = environment_path("ProgramData", "C:\\ProgramData");
    const fs::path user_profile = environment_path("USERPROFILE", program_data);
    return {
        program_files / "UCI Arena Manager",
        program_data / "UCI Arena Manager",
        program_data / "UCI Arena Manager" / "cache",
        user_profile / "UCI Arena Workspaces",
        program_data / "UCI Arena Manager" / "knowledge",
    };
#else
    const fs::path state_home =
        environment_path("XDG_STATE_HOME", environment_path("HOME", "/tmp") / ".local/state");
    return {
        "/opt/uci-arena",
        "/var/lib/uci-arena",
        "/var/cache/uci-arena",
        state_home / "uci-arena/workspaces",
        "/var/lib/uci-arena/knowledge",
    };
#endif
}

void print(
    const json& value,
    const bool machine,
    const fs::path& result_file = {}) {
    const std::string rendered = value.dump(machine ? -1 : 2) + '\n';
    std::cout << rendered;
    if (!result_file.empty()) {
        std::ofstream output(result_file, std::ios::binary | std::ios::trunc);
        if (!output) {
            throw std::runtime_error("cannot write the requested setup result file");
        }
        output << rendered;
        output.flush();
        if (!output) {
            throw std::runtime_error("cannot commit the requested setup result file");
        }
    }
}

} // namespace

int main(int argc, char** argv) {
    curl_global_init(CURL_GLOBAL_DEFAULT);
    fs::path result_file;
    try {
        CLI::App app{"Trusted cross-platform installer for UCI Arena Manager and provider "
                     "components"};
        app.require_subcommand(1);

        SetupPaths paths = default_paths();
        fs::path catalog_path{"release/catalog.json"};
        fs::path metadata_directory;
        bool allow_unsigned_development_catalog = false;
        bool json_output = false;
        auto* catalog_option = app.add_option("--catalog", catalog_path, "Installation catalog");
        auto* metadata_option =
            app.add_option("--metadata-dir", metadata_directory, "Trusted TUF metadata directory");
        app.add_flag("--allow-unsigned-development-catalog", allow_unsigned_development_catalog,
                     "Permit an unsigned local catalog; forbidden for official releases");
        app.add_option("--install-root", paths.install_root);
        app.add_option("--data-root", paths.data_root);
        app.add_option("--cache-root", paths.cache_root);
        app.add_option("--workspace-root", paths.workspace_root);
        app.add_option("--knowledge-root", paths.knowledge_root);
        app.add_option("--bundle-owner", paths.bundle_owner,
                       "Immutable calling bundle provider key");
        app.add_flag("--restart-manager-service", paths.restart_manager_service,
                     "Restart the installed Manager service after component activation");
        app.add_flag("--json", json_output, "Emit compact JSON");
        app.add_option(
            "--result-file",
            result_file,
            "Write the final machine-readable result to this helper-owned path");

        std::vector<fs::path> scan_roots;
        auto* inventory = app.add_subcommand("inventory", "Detect managed and local components");
        inventory->add_option("--scan-root", scan_roots,
                              "Additional bounded root to inspect; may be repeated");

        std::string import_kind;
        fs::path import_source;
        bool managed_copy = false;
        auto* import_command =
            app.add_subcommand("import", "Validate and save a local component locator");
        import_command
            ->add_option("--kind", import_kind,
                         "opening_book, syzygy, uci_engine, book_forge, evidence_service, "
                         "timing_profile, lichess_single_game_profile, "
                         "lichess_capacity_profile, lichess_transport_profile, or "
                         "lichess_token_path")
            ->required()
            ->check(CLI::IsMember({"opening_book", "syzygy", "uci_engine", "book_forge",
                                   "evidence_service", "timing_profile",
                                   "lichess_single_game_profile", "lichess_capacity_profile",
                                   "lichess_transport_profile", "lichess_token_path",
                                   "restaurant"}));
        import_command->add_option("--path", import_source)->required();
        import_command->add_flag("--managed-copy", managed_copy,
                                 "Copy a small opening book into Manager-owned knowledge storage");

        std::string preset{"complete_arena"};
        std::vector<std::string> components;
        bool offline = false;
        bool download_missing_knowledge = false;
        bool download_missing_books = false;
        bool download_missing_tablebases = false;
        std::vector<std::string> bundle_features;
        auto configure_plan = [&](CLI::App* command) {
            command->add_option("--preset", preset, "complete_arena, manager_only, or custom")
                ->check(CLI::IsMember({"complete_arena", "manager_only", "custom"}));
            command->add_option("--component", components,
                                "Component id for the custom preset; may be repeated");
            command->add_flag("--offline", offline, "Forbid network acquisition");
            command->add_flag("--download-missing-knowledge", download_missing_knowledge,
                              "Add the signed opening-book and Syzygy WDL components");
            command->add_flag("--download-missing-books", download_missing_books,
                              "Add the signed standard opening-book component");
            command->add_flag("--download-missing-tablebases", download_missing_tablebases,
                              "Add the signed bounded Syzygy WDL component");
            command->add_option(
                "--bundle-feature", bundle_features,
                "Installer feature selection as component-id=0 or component-id=1; "
                "may be repeated");
        };

        auto* plan_command = app.add_subcommand("plan", "Resolve dependencies and capacity");
        configure_plan(plan_command);
        bool yes = false;
        auto* apply_command = app.add_subcommand("apply", "Apply a verified installation plan");
        configure_plan(apply_command);
        apply_command->add_flag("--yes", yes, "Confirm the reviewed plan");
        auto* repair_command =
            app.add_subcommand("repair", "Reinstall and verify selected component versions");
        configure_plan(repair_command);
        repair_command->add_flag("--yes", yes, "Confirm the reviewed repair plan");
        auto* upgrade_command =
            app.add_subcommand("upgrade", "Install the catalog-selected component versions");
        configure_plan(upgrade_command);
        upgrade_command->add_flag("--yes", yes, "Confirm the reviewed upgrade plan");

        auto* verify_command =
            app.add_subcommand("verify", "Verify all managed receipts and files");

        std::vector<std::string> uninstall_components;
        bool purge = false;
        bool uninstall_all = false;
        auto* uninstall_command =
            app.add_subcommand("uninstall", "Remove managed component binaries");
        auto* uninstall_component_option = uninstall_command
                                               ->add_option("--component", uninstall_components,
                                                            "Managed component id; may be repeated")
                                               ->expected(-1);
        auto* uninstall_all_option = uninstall_command->add_flag(
            "--all", uninstall_all, "Remove every receipt-selected managed component");
        uninstall_component_option->excludes(uninstall_all_option);
        uninstall_all_option->excludes(uninstall_component_option);
        uninstall_command->add_flag("--yes", yes, "Confirm uninstall");
        uninstall_command->add_flag("--purge", purge,
                                    "Also remove only receipt-declared data paths "
                                    "under the managed data root");

        CLI11_PARSE(app, argc, argv);

        if (*inventory) {
            print(SetupEngine::inventory(paths, scan_roots), json_output, result_file);
        } else if (*import_command) {
            print(SetupEngine::import_path(paths, import_kind, import_source, managed_copy),
                  json_output, result_file);
        } else if (*verify_command) {
            const auto result = SetupEngine::verify(paths);
            print(result, json_output, result_file);
            curl_global_cleanup();
            return result.value("ok", false) ? 0 : 4;
        } else if (*uninstall_command) {
            if (!uninstall_all && uninstall_components.empty()) {
                throw std::runtime_error("uninstall requires --component or --all");
            }
            print(
                SetupEngine::uninstall(
                    paths, uninstall_components, yes, purge, uninstall_all),
                json_output,
                result_file);
        } else {
            if (catalog_option->count() == 0) {
                const fs::path retained = paths.data_root / "release" / "catalog.json";
                if (fs::is_regular_file(retained)) {
                    catalog_path = retained;
                }
            }
            if (metadata_option->count() == 0) {
                const fs::path retained = paths.data_root / "release" / "tuf";
                if (fs::is_directory(retained)) {
                    metadata_directory = retained;
                }
            }
            const json catalog = SetupEngine::load_catalog(
                catalog_path, metadata_directory, allow_unsigned_development_catalog,
                paths.data_root / "trust" / "tuf-versions.json",
                paths.data_root / "release" /
                    "development-trust.json");
            std::unordered_map<std::string, bool> feature_state{
                {"uci_arena.engine", true},
                {"book_forge.service", true},
                {"uci_arena_evidence.service", true},
                {"lichess.adapter", true},
            };
            if (!bundle_features.empty()) {
                for (const auto& feature : bundle_features) {
                    const auto separator = feature.find('=');
                    if (separator == std::string::npos ||
                        (feature.substr(separator + 1) != "0" &&
                         feature.substr(separator + 1) != "1")) {
                        throw std::runtime_error(
                            "bundle feature must be component-id=0 or component-id=1");
                    }
                    const std::string id = feature.substr(0, separator);
                    if (!feature_state.contains(id)) {
                        throw std::runtime_error("unknown bundle feature " + id);
                    }
                    feature_state.at(id) = feature.substr(separator + 1) == "1";
                }
                preset = "custom";
                components.clear();
                for (const auto& id : {"uci_arena.engine", "book_forge.service",
                                       "uci_arena_evidence.service", "lichess.adapter"}) {
                    if (feature_state.at(id)) {
                        components.emplace_back(id);
                    }
                }
            }
            const json resolved = SetupEngine::plan(
                catalog, paths, preset, components, offline, download_missing_knowledge,
                download_missing_books, download_missing_tablebases,
                repair_command->parsed() || upgrade_command->parsed());
            if (*plan_command) {
                print(resolved, json_output, result_file);
                curl_global_cleanup();
                return resolved.at("blockers").empty() ? 0 : 3;
            }
            const bool feature_reconcile = !bundle_features.empty();
            const bool restart_manager = paths.restart_manager_service;
            bool manager_was_running = false;
            if (feature_reconcile && restart_manager) {
                manager_was_running = manager_service_running();
                if (manager_was_running &&
                    arena::application::perform_service_action(
                        manager_service_name(), "stop") != 0) {
                    throw std::runtime_error(
                        "Manager service could not be stopped for component transition");
                }
                paths.restart_manager_service = false;
            }
            json result;
            try {
                result = SetupEngine::apply(catalog, resolved, paths, yes,
                                            *repair_command || *upgrade_command);
                if (feature_reconcile && result.value("state", "") == "committed") {
                    std::set<std::string> selected;
                    for (const auto& action : resolved.at("actions")) {
                        selected.insert(action.at("component_id").get<std::string>());
                    }
                    std::vector<std::string> remove;
                    for (const auto& id :
                         {"lichess.adapter", "uci_arena_evidence.service",
                          "book_forge.service", "uci_arena.engine",
                          "opening_book.standard", "stockfish.engine",
                          "node_runtime.private"}) {
                        if (!selected.contains(id) &&
                            fs::is_regular_file(
                                paths.data_root / "receipts" /
                                (std::string(id) + ".json"))) {
                            remove.emplace_back(id);
                        }
                    }
                    if (!remove.empty()) {
                        result["removed_components"] =
                            SetupEngine::uninstall(paths, remove, true, false);
                    } else {
                        result["removed_components"] = json::array();
                    }
                }
            } catch (...) {
                if (feature_reconcile && restart_manager &&
                    manager_was_running) {
                    static_cast<void>(arena::application::perform_service_action(
                        manager_service_name(), "start"));
                }
                throw;
            }
            if (feature_reconcile && restart_manager &&
                manager_was_running &&
                arena::application::perform_service_action(
                    manager_service_name(), "start") != 0) {
                throw std::runtime_error(
                    "Manager service prior running state could not be restored "
                    "after component transition");
            }
            if (feature_reconcile && restart_manager) {
                result["manager_service_state_preserved"] =
                    manager_was_running ? "running" : "stopped";
            }
            if (result.value("state", "") == "committed") {
                result["retained_release_media"] = SetupEngine::persist_release_media(
                    catalog,
                    paths,
                    catalog_path,
                    metadata_directory,
                    catalog.value("_catalog_trust", "") != "tuf");
            }
            print(result, json_output, result_file);
        }
        curl_global_cleanup();
        return 0;
    } catch (const CLI::ParseError& error) {
        curl_global_cleanup();
        return error.get_exit_code();
    } catch (const std::exception& error) {
        if (!result_file.empty()) {
            try {
                print(
                    {
                        {"schema", "arena_setup_problem_v1"},
                        {"code", "setup_failed"},
                        {"detail", error.what()},
                    },
                    true,
                    result_file);
            } catch (...) {
            }
        }
        std::cerr << "arena-setup: " << error.what() << '\n';
        curl_global_cleanup();
        return 2;
    }
}
