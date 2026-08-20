#pragma once

#include "arena/ports/client_key_store.hpp"

#include <filesystem>
#include <memory>

namespace arena::platform {

[[nodiscard]] std::shared_ptr<ports::ClientKeyStore> make_client_key_store(
    const std::filesystem::path& state_directory);

}  // namespace arena::platform
