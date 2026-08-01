#include "arena/application/installer/credential_recovery.hpp"

#include <limits>
#include <stdexcept>
#include <thread>

namespace arena::application::installer {

BookForgeCredentialRecoveryAssessment
assess_book_forge_credential_recovery(const nlohmann::json& response) {
    if (!response.is_object()) {
        return {.failure_detail = "response was not an object"};
    }
    if (!response.contains("ok") || !response.at("ok").is_boolean() ||
        !response.at("ok").get<bool>()) {
        return {.failure_detail = "ok was not true"};
    }
    if (!response.contains("retried") || (!response.at("retried").is_number_unsigned() &&
                                          !response.at("retried").is_number_integer())) {
        return {.failure_detail = "retried was not a non-negative integer"};
    }
    if (response.at("retried").is_number_integer() &&
        response.at("retried").get<std::int64_t>() < 0) {
        return {.failure_detail = "retried was not a non-negative integer"};
    }
    const auto retried = response.at("retried").get<std::uint64_t>();
    if (!response.contains("acquisition") || !response.at("acquisition").is_object()) {
        return {.retried = retried, .failure_detail = "acquisition was not an object"};
    }
    const auto& acquisition = response.at("acquisition");
    if (!acquisition.contains("credentialConfigured") ||
        !acquisition.at("credentialConfigured").is_boolean() ||
        !acquisition.at("credentialConfigured").get<bool>()) {
        return {.retried = retried, .failure_detail = "credentialConfigured was not true"};
    }
    if (!acquisition.contains("authState") || !acquisition.at("authState").is_string()) {
        return {.retried = retried, .failure_detail = "authState was not a string"};
    }
    const std::string auth_state = acquisition.at("authState").get<std::string>();
    if (auth_state == "ready") {
        return {.ready = true, .retried = retried};
    }
    if (auth_state == "blocked") {
        return {
            .retryable = true, .retried = retried, .failure_detail = "authState remained blocked"};
    }
    return {.retried = retried, .failure_detail = "authState was neither ready nor blocked"};
}

nlohmann::json recover_book_forge_credential(const BookForgeCredentialRecoveryInvoker& invoke,
                                             const int maximum_attempts,
                                             const std::chrono::milliseconds retry_delay) {
    if (!invoke || maximum_attempts < 1 || maximum_attempts > 3 ||
        retry_delay < std::chrono::milliseconds::zero() ||
        retry_delay > std::chrono::milliseconds(250)) {
        throw std::invalid_argument("Book Forge credential recovery bounds are invalid");
    }
    std::uint64_t total_retried = 0;
    for (int attempt = 1; attempt <= maximum_attempts; ++attempt) {
        const BookForgeCredentialRecoveryAssessment assessment =
            assess_book_forge_credential_recovery(invoke());
        if (assessment.retried > (std::numeric_limits<std::uint64_t>::max)() - total_retried) {
            total_retried = (std::numeric_limits<std::uint64_t>::max)();
        } else {
            total_retried += assessment.retried;
        }
        if (assessment.ready) {
            return {
                {"retried", total_retried},
                {"credential_configured", true},
                {"auth_state", "ready"},
                {"attempts", attempt},
            };
        }
        if (!assessment.retryable || attempt == maximum_attempts) {
            throw std::runtime_error("Book Forge credential recovery rejected public response: " +
                                     assessment.failure_detail + " after " +
                                     std::to_string(attempt) +
                                     (attempt == 1 ? " attempt" : " attempts"));
        }
        std::this_thread::sleep_for(retry_delay);
    }
    throw std::runtime_error("Book Forge credential recovery exhausted its bounded attempts");
}

} // namespace arena::application::installer
