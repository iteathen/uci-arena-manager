#pragma once

#include <filesystem>

namespace arena::platform {

void grant_manager_workspace_access(const std::filesystem::path& workspace);

} // namespace arena::platform
