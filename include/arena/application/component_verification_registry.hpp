#pragma once

#include <filesystem>
#include <functional>
#include <memory>
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <vector>

namespace arena::application {

struct VerifiedComponent final {
    nlohmann::json receipt;
    nlohmann::json manifest;
    std::filesystem::path active_root;
};

class ComponentVerificationUnavailable final : public std::runtime_error {
  public:
    ComponentVerificationUnavailable(std::string code, std::string detail);

    [[nodiscard]] const std::string& code() const noexcept;

  private:
    std::string code_;
};

class ComponentVerificationRegistry final {
  public:
    using VerifyFunction =
        std::function<VerifiedComponent(const std::filesystem::path&, const std::string&, bool)>;

    explicit ComponentVerificationRegistry(VerifyFunction verifier = {});
    ~ComponentVerificationRegistry();

    ComponentVerificationRegistry(const ComponentVerificationRegistry&) = delete;
    ComponentVerificationRegistry& operator=(const ComponentVerificationRegistry&) = delete;

    void begin(const std::filesystem::path& manager_data_root);
    void stop();
    [[nodiscard]] bool started() const;
    [[nodiscard]] bool wait_for_initial_verification(std::stop_token stop_token = {}) const;
    [[nodiscard]] bool wait_for_current_verification(std::stop_token stop_token = {}) const;
    [[nodiscard]] nlohmann::json inventory() const;
    [[nodiscard]] VerifiedComponent require_verified(const std::string& component_id,
                                                     bool allow_disabled = false) const;
    [[nodiscard]] std::vector<VerifiedComponent> verified_components() const;

    void invalidate_all_for_testing();
    [[nodiscard]] std::size_t verification_attempts_for_testing() const;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace arena::application
