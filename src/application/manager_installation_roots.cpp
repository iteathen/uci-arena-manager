#include "arena/application/manager_installation_roots.hpp"
#include "arena/application/process_supervisor.hpp"
#include "arena/platform/path_inspection.hpp"

#include <array>
#include <chrono>
#include <map>
#include <stdexcept>
#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#endif

namespace arena::application {
namespace {
using nlohmann::json;
constexpr std::array<std::string_view, 5> names{"data", "install", "cache", "workspace", "knowledge"};
constexpr std::array<std::string_view, 5> flags{"--data-dir", "--install-root", "--cache-root", "--workspace-root", "--knowledge-root"};
[[noreturn]] void invalid() { throw std::runtime_error("Manager installation roots do not match admitted Installer authority"); }
std::filesystem::path absolute_root(const std::string& text) {
    if (text.empty() || text.size() > 32767 || text.find_first_of("\r\n\"") != std::string::npos || text.find('\0') != std::string::npos) invalid();
    auto result = std::filesystem::path(text).lexically_normal();
    if (!result.is_absolute()) invalid();
    while (result.has_relative_path() && result.filename().empty()) result = result.parent_path();
    return result;
}
std::array<std::filesystem::path, 5> paths(const ManagerInstallationRoots& roots) {
    return {roots.data, roots.install, roots.cache, roots.workspace, roots.knowledge};
}
ManagerInstallationRoots roots_from(const std::array<std::filesystem::path, 5>& values) {
    return {values[0], values[1], values[2], values[3], values[4]};
}
template <std::size_t N> void exact_keys(const json& value, const std::array<std::string_view, N>& fields) {
    if (!value.is_object() || value.size() != N) invalid();
    for (const auto field : fields) if (!value.contains(field)) invalid();
}
bool equal_path(const std::filesystem::path& left, const std::filesystem::path& right) {
    const auto a = absolute_root(left.string()), b = absolute_root(right.string());
#ifdef _WIN32
    return CompareStringOrdinal(a.c_str(), -1, b.c_str(), -1, TRUE) == CSTR_EQUAL;
#else
    return a == b;
#endif
}
}

std::vector<std::string> manager_root_arguments(const ManagerInstallationRoots& roots) {
    std::vector<std::string> result; const auto values = paths(roots);
    for (std::size_t i = 0; i < names.size(); ++i) {
        result.emplace_back(flags[i]); result.push_back(absolute_root(values[i].string()).string());
    }
    return result;
}
std::vector<std::string> manager_setup_root_arguments(const ManagerInstallationRoots& roots) {
    auto result = manager_root_arguments(roots); result[0] = "--data-root"; return result;
}
std::vector<std::string> manager_setup_root_arguments(const std::optional<ManagerInstallationRoots>& roots,
    const std::filesystem::path& legacy_data) {
    return roots ? manager_setup_root_arguments(*roots) :
        std::vector<std::string>{"--data-root", legacy_data.string()};
}
std::vector<std::string> manager_service_arguments(const std::filesystem::path& data, const std::filesystem::path& web) {
    return {"--data-dir", absolute_root(data.string()).string(), "--web-root", absolute_root(web.string()).string()};
}
std::filesystem::path manager_browser_profile_root(const std::filesystem::path& data, const bool per_user,
    const std::filesystem::path& private_user_profile) {
    return per_user ? absolute_root(data.string()) / "WebView2" : absolute_root(private_user_profile.string());
}
std::filesystem::path manager_helper_scratch_parent(const std::optional<ManagerInstallationRoots>& roots, const bool per_user,
    const std::filesystem::path& private_user_temp) {
    return roots && per_user ? absolute_root(roots->cache.string()) : absolute_root(private_user_temp.string());
}
std::optional<ManagerInstallationRoots> parse_desktop_manager_roots(const std::vector<std::string>& arguments) {
    if (arguments.empty()) return std::nullopt;
    if (arguments.size() != flags.size() * 2) invalid();
    std::map<std::string, std::string, std::less<>> supplied;
    for (std::size_t i = 0; i < arguments.size(); i += 2) if (!supplied.emplace(arguments[i], arguments[i + 1]).second) invalid();
    std::array<std::filesystem::path, 5> values;
    for (std::size_t i = 0; i < flags.size(); ++i) {
        const auto found = supplied.find(flags[i]); if (found == supplied.end()) invalid();
        values[i] = absolute_root(found->second);
    }
    return roots_from(values);
}
ManagerInstallationRoots admit_manager_layout_selection(const json& document, const std::string_view scope) {
    exact_keys(document, std::array<std::string_view, 4>{"schema", "scope", "integration_root", "roots"});
    if ((scope != "per-user" && scope != "per-machine") || document.at("schema") != "arena_package_layout_selection_v1" ||
        !document.at("scope").is_string() || document.at("scope").get<std::string>() != scope || !document.at("integration_root").is_string()) invalid();
    static_cast<void>(absolute_root(document.at("integration_root").get<std::string>()));
    exact_keys(document.at("roots"), names);
    std::array<std::filesystem::path, 5> values;
    for (std::size_t i = 0; i < names.size(); ++i) {
        const auto& value = document.at("roots").at(names[i]); if (!value.is_string()) invalid();
        values[i] = absolute_root(value.get<std::string>());
    }
    return roots_from(values);
}
std::optional<ManagerInstallationRoots> resolve_manager_installation_roots(
    const std::optional<ManagerInstallationRoots>& requested, const ManagerLayoutQuery& query, const std::string_view scope) {
    const auto document = query();
    if (!document) { if (requested) invalid(); return std::nullopt; }
    const auto admitted = admit_manager_layout_selection(*document, scope);
    if (requested) {
        const auto selected = paths(admitted), supplied = paths(*requested);
        for (std::size_t i = 0; i < names.size(); ++i) if (!equal_path(selected[i], supplied[i])) invalid();
    }
    return admitted;
}
std::optional<json> query_manager_package_layout(const std::filesystem::path& setup, const std::string_view scope) {
    if (scope != "per-user" && scope != "per-machine") invalid();
    const auto executable = absolute_root(setup.string());
    auto prefix = executable.root_path();
    for (const auto& part : executable.relative_path()) {
        prefix /= part;
        const auto observation = platform::inspect_path_no_follow(prefix);
        if (prefix == executable && observation.kind == platform::PathEntryKind::missing) return std::nullopt;
        if (prefix != executable && observation.kind != platform::PathEntryKind::directory) invalid();
        if (prefix == executable && (observation.kind != platform::PathEntryKind::regular_file || !observation.executable ||
            observation.size_bytes == 0 || observation.size_bytes > 256ULL * 1024 * 1024)) invalid();
    }
    const auto before = platform::inspect_path_no_follow(executable);
    ProcessSupervisor process;
    const auto result = process.run_json(executable, executable.parent_path(),
        {"--install-scope", std::string(scope), "--json", "layout"}, std::chrono::seconds(15), 1024 * 1024, true);
    const auto after = platform::inspect_path_no_follow(executable);
    if (after.kind != platform::PathEntryKind::regular_file || before.object_identity != after.object_identity || before.size_bytes != after.size_bytes) invalid();
    return std::optional<json>{std::in_place, result};
}
}
