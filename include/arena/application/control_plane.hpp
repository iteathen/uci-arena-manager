#pragma once

#include "arena/application/component_verification_registry.hpp"
#include "arena/ports/store.hpp"
#include "arena/application/process_supervisor.hpp"
#include "arena/application/tournament_executor.hpp"

#include <filesystem>
#include <memory>
#include <mutex>
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <utility>
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
    void begin_installed_reconciliation(
        const std::filesystem::path& manager_data_root);
    void reconcile_installed_services(
        const std::filesystem::path& manager_data_root,
        std::stop_token stop_token = {});
    [[nodiscard]] nlohmann::json status() const;
    [[nodiscard]] nlohmann::json readiness() const;
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
    [[nodiscard]] nlohmann::json engine_candidates(
        const std::vector<std::filesystem::path>& explicit_roots = {}) const;
    [[nodiscard]] nlohmann::json component_inventory() const;
    [[nodiscard]] nlohmann::json bot_profiles() const;
    [[nodiscard]] nlohmann::json bot_candidates() const;
    [[nodiscard]] ReportArtifact report_artifact(
        const std::string& report_id,
        const std::string& format) const;

  private:
    void reconcile_installed_engine();
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
    void set_installed_service_reconciliation(nlohmann::json value);
    void set_installed_engine_reconciliation(nlohmann::json value);
    [[nodiscard]] std::pair<nlohmann::json, nlohmann::json>
    startup_reconciliation_snapshot() const;

    std::shared_ptr<ports::Store> store_;
    std::mutex engine_authority_mutex_;
    std::mutex reconciliation_run_mutex_;
    mutable std::mutex reconciliation_state_mutex_;
    std::filesystem::path manager_data_root_;
    std::shared_ptr<ComponentVerificationRegistry> component_registry_;
    ProcessSupervisor process_supervisor_;
    TournamentExecutor tournament_executor_;
    std::string started_at_;
    nlohmann::json installed_service_reconciliation_{
        {"schema", "arena_installed_service_reconciliation_v1"},
        {"state", "not_run"}};
    nlohmann::json installed_engine_reconciliation_{
        {"schema", "arena_installed_engine_reconciliation_v1"},
        {"state", "not_run"}};
};

[[nodiscard]] nlohmann::json problem_json(const CommandFailure& failure);

}  // namespace arena::application
