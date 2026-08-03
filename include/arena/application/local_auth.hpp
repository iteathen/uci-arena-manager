#pragma once

#include <chrono>
#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>

namespace arena::application {

struct BrowserSession {
    std::string id;
    std::string csrf_token;
};

class LocalAuth final {
  public:
    explicit LocalAuth(std::filesystem::path state_directory);

    void initialize();
    [[nodiscard]] const std::filesystem::path& client_key_path() const;
    [[nodiscard]] std::string server_proof(
        const std::string& challenge,
        const std::string& audience) const;
    [[nodiscard]] static std::string compute_server_proof(
        const std::string& client_key,
        const std::string& challenge,
        const std::string& audience);
    [[nodiscard]] bool authenticate_bearer(const std::string& authorization) const;
    [[nodiscard]] std::string issue_pairing_token();
    [[nodiscard]] std::optional<BrowserSession> exchange_pairing_token(
        const std::string& token);
    [[nodiscard]] std::optional<BrowserSession> resume_browser_session(
        const std::string& session_id) const;
    [[nodiscard]] bool authenticate_session(const std::string& session_id) const;
    [[nodiscard]] bool validate_csrf(
        const std::string& session_id,
        const std::string& csrf_token) const;

  private:
    struct ExpiringValue {
        std::string value;
        std::chrono::steady_clock::time_point expires_at;
    };

    [[nodiscard]] static std::string random_token();
    void prune_expired();

    std::filesystem::path state_directory_;
    std::filesystem::path client_key_path_;
    mutable std::string client_key_;
    mutable std::mutex mutex_;
    bool initialized_{};
    std::unordered_map<std::string, ExpiringValue> pairing_tokens_;
    std::unordered_map<std::string, ExpiringValue> sessions_;
};

}  // namespace arena::application
