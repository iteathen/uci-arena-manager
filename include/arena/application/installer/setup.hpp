#pragma once

#include <filesystem>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

namespace arena::application::installer {

struct SetupPaths {
    std::filesystem::path install_root;
    std::filesystem::path data_root;
    std::filesystem::path cache_root;
    std::filesystem::path workspace_root;
    std::filesystem::path knowledge_root;
    std::string bundle_owner;
    bool restart_manager_service{};
    bool manager_service_access{true};
};

class SetupEngine final {
  public:
    [[nodiscard]] static std::string platform_key();

    [[nodiscard]] static nlohmann::json
    load_catalog(const std::filesystem::path& catalog_path,
                 const std::filesystem::path& metadata_directory = {},
                 bool allow_unsigned_development_catalog = false,
                 const std::filesystem::path& rollback_state_path = {},
                 const std::filesystem::path& retained_development_trust_path = {});

    [[nodiscard]] static nlohmann::json
    persist_release_media(const nlohmann::json& catalog, const SetupPaths& paths,
                          const std::filesystem::path& catalog_path,
                          const std::filesystem::path& metadata_directory = {},
                          bool retain_development_trust = false);

    [[nodiscard]] static nlohmann::json
    inventory(const SetupPaths& paths, const std::vector<std::filesystem::path>& extra_roots = {});

    [[nodiscard]] static nlohmann::json import_path(const SetupPaths& paths,
                                                    const std::string& kind,
                                                    const std::filesystem::path& path,
                                                    bool managed_copy = false);

    [[nodiscard]] static nlohmann::json save_lichess_token_reference(
        const std::filesystem::path& data_root,
        const std::filesystem::path& path);

    [[nodiscard]] static nlohmann::json
    plan(const nlohmann::json& catalog, const SetupPaths& paths, const std::string& preset,
         const std::vector<std::string>& selected_components, bool offline,
         bool download_missing_knowledge, bool download_missing_books = false,
         bool download_missing_tablebases = false, bool repair = false);

    [[nodiscard]] static nlohmann::json apply(const nlohmann::json& catalog,
                                              const nlohmann::json& plan, const SetupPaths& paths,
                                              bool confirmed, bool repair);

    [[nodiscard]] static nlohmann::json verify(const SetupPaths& paths);

    [[nodiscard]] static nlohmann::json uninstall(const SetupPaths& paths,
                                                  const std::vector<std::string>& component_ids,
                                                  bool confirmed, bool purge, bool all = false);
};

} // namespace arena::application::installer
