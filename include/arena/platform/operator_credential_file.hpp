#pragma once

#include <filesystem>
#include <string_view>

namespace arena::platform {

[[nodiscard]] std::filesystem::path default_operator_credential_root();

class OperatorCredentialFileUpdate final {
  public:
    OperatorCredentialFileUpdate(
        std::filesystem::path destination,
        std::string_view contents);
    ~OperatorCredentialFileUpdate();

    OperatorCredentialFileUpdate(const OperatorCredentialFileUpdate&) = delete;
    OperatorCredentialFileUpdate& operator=(
        const OperatorCredentialFileUpdate&) = delete;
    OperatorCredentialFileUpdate(OperatorCredentialFileUpdate&&) = delete;
    OperatorCredentialFileUpdate& operator=(
        OperatorCredentialFileUpdate&&) = delete;

    [[nodiscard]] const std::filesystem::path& path() const noexcept;
    void commit();

  private:
    void rollback() noexcept;

    std::filesystem::path destination_;
    std::filesystem::path staged_;
    std::filesystem::path backup_;
    bool had_previous_{};
    bool active_{};
};

}  // namespace arena::platform
