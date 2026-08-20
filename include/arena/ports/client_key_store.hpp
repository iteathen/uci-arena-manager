#pragma once

#include <stdexcept>
#include <string>
#include <string_view>

namespace arena::ports {

enum class ClientKeyFailureKind {
    missing,
    invalid,
    unsafe,
    io,
};

class ClientKeyFailure final : public std::runtime_error {
  public:
    explicit ClientKeyFailure(const ClientKeyFailureKind kind)
        : std::runtime_error("client authentication key is unavailable"),
          kind_(kind) {}

    [[nodiscard]] ClientKeyFailureKind kind() const noexcept { return kind_; }

  private:
    ClientKeyFailureKind kind_;
};

struct ClientKeyMaterial final {
    std::string value;
    std::string object_identity;
};

class ClientKeyStore {
  public:
    virtual ~ClientKeyStore() = default;

    [[nodiscard]] virtual ClientKeyMaterial load_existing() const = 0;
    [[nodiscard]] virtual ClientKeyMaterial load_or_create(
        std::string_view proposed_key) = 0;
};

}  // namespace arena::ports
