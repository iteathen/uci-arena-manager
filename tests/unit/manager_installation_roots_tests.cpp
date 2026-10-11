#include "arena/application/manager_installation_roots.hpp"
#include "arena/domain/uuid.hpp"

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>
#include <filesystem>
#include <string>
#include <vector>

namespace {
using namespace arena::application;
using nlohmann::json;
ManagerInstallationRoots fixture_roots() {
    const auto base = std::filesystem::absolute("Selected Suite With Spaces");
    return {base / "data", base / "installed", base / "cache", base / "workspaces", base / "knowledge"};
}
json selection(const ManagerInstallationRoots& roots) {
    return {{"schema", "arena_package_layout_selection_v1"}, {"scope", "per-user"},
        {"integration_root", roots.install.parent_path().string()},
        {"roots", {{"data", roots.data.string()}, {"install", roots.install.string()},
            {"cache", roots.cache.string()}, {"workspace", roots.workspace.string()}, {"knowledge", roots.knowledge.string()}}}};
}
}

TEST_CASE("Desktop consumes a complete selected root tuple and setup receives every root", "[manager-roots]") {
    const auto expected = fixture_roots();
    const auto arguments = manager_root_arguments(expected);
    REQUIRE(parse_desktop_manager_roots(arguments) == expected);
    const auto setup = manager_setup_root_arguments(expected);
    REQUIRE(setup == std::vector<std::string>{"--data-root", expected.data.string(), "--install-root", expected.install.string(),
        "--cache-root", expected.cache.string(), "--workspace-root", expected.workspace.string(), "--knowledge-root", expected.knowledge.string()});
    REQUIRE(manager_service_arguments(expected.data, expected.install / "web") ==
        std::vector<std::string>{"--data-dir", expected.data.string(), "--web-root", (expected.install / "web").string()});
    REQUIRE_FALSE(parse_desktop_manager_roots({}).has_value());
}

TEST_CASE("Legacy Linux setup forwards only its data root without Windows selection", "[manager-roots]") {
    const auto roots = fixture_roots();
    REQUIRE(manager_setup_root_arguments(std::nullopt, roots.data) ==
        std::vector<std::string>{"--data-root", roots.data.string()});
    REQUIRE(manager_setup_root_arguments(roots, roots.data) == manager_setup_root_arguments(roots));
}

TEST_CASE("Partial duplicate relative and unknown desktop roots cannot start a selected installation", "[manager-roots]") {
    auto arguments = manager_root_arguments(fixture_roots());
    REQUIRE_THROWS(parse_desktop_manager_roots({"--data-dir", arguments[1]}));
    auto incomplete = arguments; incomplete.pop_back(); REQUIRE_THROWS(parse_desktop_manager_roots(incomplete));
    auto duplicate = arguments; duplicate.insert(duplicate.end(), {"--data-dir", arguments[1]}); REQUIRE_THROWS(parse_desktop_manager_roots(duplicate));
    auto relative = arguments; relative[1] = "relative/data"; REQUIRE_THROWS(parse_desktop_manager_roots(relative));
    auto unknown = arguments; unknown[0] = "--arbitrary-root"; REQUIRE_THROWS(parse_desktop_manager_roots(unknown));
}

TEST_CASE("Installer selection is the sole authority and explicit roots must agree", "[manager-roots]") {
    const auto expected = fixture_roots(); const auto document = selection(expected);
    REQUIRE(admit_manager_layout_selection(document, "per-user") == expected);
    const auto query = [&]() -> std::optional<json> { return std::optional<json>{std::in_place, document}; };
    REQUIRE(resolve_manager_installation_roots(std::nullopt, query, "per-user") == expected);
    REQUIRE(resolve_manager_installation_roots(expected, query, "per-user") == expected);
    auto foreign = expected; foreign.workspace /= "foreign";
    REQUIRE_THROWS(resolve_manager_installation_roots(foreign, query, "per-user"));
    REQUIRE_THROWS(admit_manager_layout_selection(document, "per-machine"));
    const auto missing = []() -> std::optional<json> { return std::nullopt; };
    REQUIRE_FALSE(resolve_manager_installation_roots(std::nullopt, missing, "per-user"));
    REQUIRE_THROWS(resolve_manager_installation_roots(expected, missing, "per-user"));
    const auto failure = []() -> std::optional<json> { throw std::runtime_error("selection invalid"); };
    REQUIRE_THROWS(resolve_manager_installation_roots(std::nullopt, failure, "per-user"));
}

TEST_CASE("Malformed Installer root authority fails without a legacy root fallback", "[manager-roots]") {
    const auto valid = selection(fixture_roots());
    for (const std::string field : {"schema", "scope", "integration_root", "roots"}) {
        auto missing = valid; missing.erase(field); REQUIRE_THROWS(admit_manager_layout_selection(missing, "per-user"));
    }
    auto extra = valid; extra["credential"] = "unrecognized"; REQUIRE_THROWS(admit_manager_layout_selection(extra, "per-user"));
    auto relative = valid; relative["roots"]["data"] = "relative"; REQUIRE_THROWS(admit_manager_layout_selection(relative, "per-user"));
    auto duplicate = valid; duplicate["roots"]["other"] = "unknown"; REQUIRE_THROWS(admit_manager_layout_selection(duplicate, "per-user"));
    auto problem = json{{"schema", "arena_package_layout_selection_error_v1"}, {"code", "selection_unavailable"}};
    REQUIRE_THROWS(admit_manager_layout_selection(problem, "per-user"));
}

TEST_CASE("Public layout query sends fixed global flags before the command and joins its child", "[manager-roots-process]") {
    const auto result = query_manager_package_layout(ARENA_MOCK_SERVICE_PATH, "per-user");
    REQUIRE(result.has_value());
    const auto roots = admit_manager_layout_selection(*result, "per-user");
    REQUIRE(roots.data.is_absolute());
    REQUIRE_FALSE(query_manager_package_layout(std::string(ARENA_MOCK_SERVICE_PATH) + ".missing", "per-user"));
    REQUIRE_THROWS(query_manager_package_layout(ARENA_MOCK_SERVICE_PATH, "foreign-scope"));
}

TEST_CASE("Per-user profile and scratch stay on selected data and cache roots", "[manager-roots]") {
    const auto roots = fixture_roots(); const auto private_profile = roots.data.parent_path() / "private-user-profile";
    const auto private_temp = roots.data.parent_path() / "private-user-temp";
    REQUIRE(manager_browser_profile_root(roots.data, true, private_profile) == roots.data / "WebView2");
    REQUIRE(manager_browser_profile_root(roots.data, false, private_profile) == private_profile);
    REQUIRE(manager_helper_scratch_parent(roots, true, private_temp) == roots.cache);
    REQUIRE(manager_helper_scratch_parent(roots, false, private_temp) == private_temp);
    REQUIRE(manager_helper_scratch_parent(std::nullopt, true, private_temp) == private_temp);
}

TEST_CASE("A failed public layout child cannot admit success-shaped JSON", "[manager-roots-process]") {
    const auto owned = std::filesystem::temp_directory_path() / ("arena-layout-status-" + arena::domain::uuid_v7());
    std::filesystem::create_directory(owned);
    const auto facade = owned / "layout-exit2-facade.exe";
    std::filesystem::copy_file(ARENA_MOCK_SERVICE_PATH, facade);
    REQUIRE_THROWS(query_manager_package_layout(facade, "per-user"));
    std::filesystem::remove(facade);
    std::filesystem::remove(owned);
}
