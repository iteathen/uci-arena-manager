#pragma once

#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>
#include <nlohmann/json.hpp>

namespace arena::application {
struct ManagerInstallationRoots {
    std::filesystem::path data, install, cache, workspace, knowledge;
    bool operator==(const ManagerInstallationRoots&) const = default;
};
using ManagerLayoutQuery = std::function<std::optional<nlohmann::json>()>;
[[nodiscard]] std::optional<ManagerInstallationRoots> parse_desktop_manager_roots(const std::vector<std::string>& arguments);
[[nodiscard]] ManagerInstallationRoots admit_manager_layout_selection(const nlohmann::json& document, std::string_view scope);
[[nodiscard]] std::optional<ManagerInstallationRoots> resolve_manager_installation_roots(
    const std::optional<ManagerInstallationRoots>& requested, const ManagerLayoutQuery& query, std::string_view scope);
[[nodiscard]] std::optional<nlohmann::json> query_manager_package_layout(const std::filesystem::path& setup, std::string_view scope);
[[nodiscard]] std::vector<std::string> manager_root_arguments(const ManagerInstallationRoots& roots);
[[nodiscard]] std::vector<std::string> manager_setup_root_arguments(const ManagerInstallationRoots& roots);
[[nodiscard]] std::vector<std::string> manager_setup_root_arguments(
    const std::optional<ManagerInstallationRoots>& roots, const std::filesystem::path& legacy_data);
[[nodiscard]] std::vector<std::string> manager_service_arguments(const std::filesystem::path& data, const std::filesystem::path& web);
[[nodiscard]] std::filesystem::path manager_browser_profile_root(const std::filesystem::path& data, bool per_user,
    const std::filesystem::path& private_user_profile);
[[nodiscard]] std::filesystem::path manager_helper_scratch_parent(const std::optional<ManagerInstallationRoots>& roots,
    bool per_user, const std::filesystem::path& private_user_temp);
}
