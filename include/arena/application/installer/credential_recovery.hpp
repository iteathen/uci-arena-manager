#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <nlohmann/json.hpp>
#include <string>

namespace arena::application::installer {

struct BookForgeCredentialRecoveryAssessment {
    bool ready{};
    bool retryable{};
    std::uint64_t retried{};
    std::string failure_detail;
};

[[nodiscard]] BookForgeCredentialRecoveryAssessment
assess_book_forge_credential_recovery(const nlohmann::json& response);

using BookForgeCredentialRecoveryInvoker = std::function<nlohmann::json()>;

[[nodiscard]] nlohmann::json
recover_book_forge_credential(const BookForgeCredentialRecoveryInvoker& invoke,
                              int maximum_attempts, std::chrono::milliseconds retry_delay);

} // namespace arena::application::installer
