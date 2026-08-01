#pragma once

#include "arena/ports/store.hpp"
#include "arena/application/process_supervisor.hpp"
#include "arena/application/tournament_executor.hpp"

#include <filesystem>
#include <memory>
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <string>
#include <vector>

namespace arena::application {

struct ReportArtifact final {
    std::string content;
    std::string media_type;
    std::string filename;
};

class CommandFailure final : public std::runtime_error {
  public:
    CommandFailure(std::string code, std::string detail, int http_status);

    [[nodiscard]] const std::string& code() const noexcept;
    [[nodiscard]] int http_status() const noexcept;

  private:
    std::string code_;
    int http_status_;
};

class ControlPlane final {
  public:
    explicit ControlPlane(std::shared_ptr<ports::Store> store);

    void initialize();
    void reconcile_installed_services(
        const std::filesystem::path& manager_data_root);
    [[nodiscard]] nlohmann::json status() const;
    [[nodiscard]] nlohmann::json submit(
        const nlohmann::json& request,
        const std::string& actor);
    [[nodiscard]] nlohmann::json list(
        const std::string& kind,
        std::uint32_t limit = 200,
        std::uint32_t offset = 0) const;
    [[nodiscard]] nlohmann::json events_after(
        std::uint64_t cursor,
        std::uint32_t limit = 500) const;
    [[nodiscard]] nlohmann::json service_profiles() const;
    [[nodiscard]] nlohmann::json discover_service_profile(
        const std::string& profile_id) const;
    [[nodiscard]] nlohmann::json component_inventory() const;
    [[nodiscard]] nlohmann::json bot_profiles() const;
    [[nodiscard]] nlohmann::json bot_candidates() const;
    [[nodiscard]] nlohmann::json save_bot_token_reference(
        const std::filesystem::path& path) const;
    [[nodiscard]] ReportArtifact report_artifact(
        const std::string& report_id,
        const std::string& format) const;

  private:
    [[nodiscard]] nlohmann::json register_engine(
        const nlohmann::json& request,
        const std::string& actor);
    [[nodiscard]] nlohmann::json plan_match(
        const nlohmann::json& request,
        const std::string& actor);
    [[nodiscard]] nlohmann::json register_generic_resource(
        const nlohmann::json& request,
        const std::string& actor);
    [[nodiscard]] nlohmann::json remove_resource(
        const nlohmann::json& request,
        const std::string& actor);
    [[nodiscard]] nlohmann::json manage_service(
        const nlohmann::json& request,
        const std::string& actor);
    [[nodiscard]] nlohmann::json manage_competition(
        const nlohmann::json& request,
        const std::string& actor);
    [[nodiscard]] nlohmann::json generate_report(
        const nlohmann::json& request,
        const std::string& actor);

    [[nodiscard]] static nlohmann::json resource_json(const ports::ResourceRecord& record);
    [[nodiscard]] static nlohmann::json event_json(const ports::EventRecord& record);
    [[nodiscard]] static nlohmann::json validate_envelope(const nlohmann::json& request);
    [[nodiscard]] nlohmann::json complete_create(
        const nlohmann::json& request,
        const std::string& actor,
        ports::ResourceRecord resource);
    [[nodiscard]] nlohmann::json complete_update(
        const nlohmann::json& request,
        const std::string& actor,
        ports::ResourceRecord resource,
        std::uint64_t expected_revision,
        const std::string& event_kind);

    std::shared_ptr<ports::Store> store_;
    std::filesystem::path manager_data_root_;
    ProcessSupervisor process_supervisor_;
    TournamentExecutor tournament_executor_;
    std::string started_at_;
    nlohmann::json installed_service_reconciliation_{
        {"schema", "arena_installed_service_reconciliation_v1"},
        {"state", "not_run"}};
};

[[nodiscard]] nlohmann::json problem_json(const CommandFailure& failure);

}  // namespace arena::application
