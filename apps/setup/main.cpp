#include "arena/application/installer/setup.hpp"

#include <CLI/CLI.hpp>
#include <curl/curl.h>
#include <nlohmann/json.hpp>

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

namespace fs = std::filesystem;
using arena::application::installer::SetupEngine;
using arena::application::installer::SetupPaths;
using nlohmann::json;

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

void print(const json& value, const bool machine) {
    std::cout << value.dump(machine ? -1 : 2) << '\n';
}

}  // namespace

int main(int argc, char** argv) {
    curl_global_init(CURL_GLOBAL_DEFAULT);
    try {
        CLI::App app{
            "Trusted cross-platform installer for UCI Arena Manager and provider components"};
        app.require_subcommand(1);

        SetupPaths paths = default_paths();
        fs::path catalog_path{"release/catalog.json"};
        fs::path metadata_directory;
        bool allow_unsigned_development_catalog = false;
        bool json_output = false;
        auto* catalog_option =
            app.add_option("--catalog", catalog_path, "Installation catalog");
        auto* metadata_option =
            app.add_option(
                "--metadata-dir",
                metadata_directory,
                "Trusted TUF metadata directory");
        app.add_flag(
            "--allow-unsigned-development-catalog",
            allow_unsigned_development_catalog,
            "Permit an unsigned local catalog; forbidden for official releases");
        app.add_option("--install-root", paths.install_root);
        app.add_option("--data-root", paths.data_root);
        app.add_option("--cache-root", paths.cache_root);
        app.add_option("--workspace-root", paths.workspace_root);
        app.add_option("--knowledge-root", paths.knowledge_root);
        app.add_flag("--json", json_output, "Emit compact JSON");

        std::vector<fs::path> scan_roots;
        auto* inventory = app.add_subcommand("inventory", "Detect managed and local components");
        inventory->add_option(
            "--scan-root",
            scan_roots,
            "Additional bounded root to inspect; may be repeated");

        std::string import_kind;
        fs::path import_source;
        bool managed_copy = false;
        auto* import_command =
            app.add_subcommand("import", "Validate and save a local component locator");
        import_command
            ->add_option(
                "--kind",
                import_kind,
                "opening_book, syzygy, uci_engine, book_forge, evidence_service, timing_profile, or lichess_token_path")
            ->required()
            ->check(CLI::IsMember(
                {"opening_book",
                 "syzygy",
                 "uci_engine",
                 "book_forge",
                 "evidence_service",
                 "timing_profile",
                 "lichess_token_path",
                 "restaurant"}));
        import_command->add_option("--path", import_source)->required();
        import_command->add_flag(
            "--managed-copy",
            managed_copy,
            "Copy a small opening book into Manager-owned knowledge storage");

        std::string preset{"complete_arena"};
        std::vector<std::string> components;
        bool offline = false;
        bool download_missing_knowledge = false;
        bool download_missing_books = false;
        bool download_missing_tablebases = false;
        auto configure_plan = [&](CLI::App* command) {
            command->add_option(
                       "--preset",
                       preset,
                       "complete_arena, manager_only, or custom")
                ->check(CLI::IsMember({"complete_arena", "manager_only", "custom"}));
            command->add_option(
                "--component",
                components,
                "Component id for the custom preset; may be repeated");
            command->add_flag("--offline", offline, "Forbid network acquisition");
            command->add_flag(
                "--download-missing-knowledge",
                download_missing_knowledge,
                "Add the signed opening-book and Syzygy WDL components");
            command->add_flag(
                "--download-missing-books",
                download_missing_books,
                "Add the signed standard opening-book component");
            command->add_flag(
                "--download-missing-tablebases",
                download_missing_tablebases,
                "Add the signed bounded Syzygy WDL component");
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

        auto* verify_command = app.add_subcommand("verify", "Verify all managed receipts and files");

        std::vector<std::string> uninstall_components;
        bool purge = false;
        auto* uninstall_command =
            app.add_subcommand("uninstall", "Remove managed component binaries");
        uninstall_command->add_option("--component", uninstall_components)
            ->required()
            ->expected(-1);
        uninstall_command->add_flag("--yes", yes, "Confirm uninstall");
        uninstall_command->add_flag(
            "--purge",
            purge,
            "Also remove only receipt-declared data paths under the managed data root");

        CLI11_PARSE(app, argc, argv);

        if (*inventory) {
            print(SetupEngine::inventory(paths, scan_roots), json_output);
        } else if (*import_command) {
            print(
                SetupEngine::import_path(
                    paths, import_kind, import_source, managed_copy),
                json_output);
        } else if (*verify_command) {
            const auto result = SetupEngine::verify(paths);
            print(result, json_output);
            curl_global_cleanup();
            return result.value("ok", false) ? 0 : 4;
        } else if (*uninstall_command) {
            print(
                SetupEngine::uninstall(paths, uninstall_components, yes, purge),
                json_output);
        } else {
            if (catalog_option->count() == 0) {
                const fs::path retained =
                    paths.data_root / "release" / "catalog.json";
                if (fs::is_regular_file(retained)) {
                    catalog_path = retained;
                }
            }
            if (metadata_option->count() == 0) {
                const fs::path retained =
                    paths.data_root / "release" / "tuf";
                if (fs::is_directory(retained)) {
                    metadata_directory = retained;
                }
            }
            const json catalog = SetupEngine::load_catalog(
                catalog_path,
                metadata_directory,
                allow_unsigned_development_catalog,
                paths.data_root / "trust" / "tuf-versions.json");
            const json resolved = SetupEngine::plan(
                catalog,
                paths,
                preset,
                components,
                offline,
                download_missing_knowledge,
                download_missing_books,
                download_missing_tablebases);
            if (*plan_command) {
                print(resolved, json_output);
                curl_global_cleanup();
                return resolved.at("blockers").empty() ? 0 : 3;
            }
            json result = SetupEngine::apply(
                catalog,
                resolved,
                paths,
                yes,
                *repair_command || *upgrade_command);
            if (result.value("state", "") == "committed") {
                result["retained_release_media"] =
                    SetupEngine::persist_release_media(
                        catalog,
                        paths,
                        catalog_path,
                        metadata_directory);
            }
            print(result, json_output);
        }
        curl_global_cleanup();
        return 0;
    } catch (const CLI::ParseError& error) {
        curl_global_cleanup();
        return error.get_exit_code();
    } catch (const std::exception& error) {
        std::cerr << "arena-setup: " << error.what() << '\n';
        curl_global_cleanup();
        return 2;
    }
}
