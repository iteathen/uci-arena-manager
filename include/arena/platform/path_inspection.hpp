#pragma once

#include <cstdint>
#include <filesystem>
#include <string>

namespace arena::platform {

enum class PathEntryKind {
    regular_file,
    directory,
    link_or_reparse,
    other,
    missing,
    inaccessible,
};

struct PathObservation final {
    PathEntryKind kind{PathEntryKind::inaccessible};
    std::uintmax_t size_bytes{0};
    bool executable{false};
    std::string object_identity;
    std::uint32_t platform_error{0};
};

// Nondestructive, no-follow observation. The returned identity is evidence for
// this observation only; callers must never retain it as launch authority.
[[nodiscard]] PathObservation inspect_path_no_follow(
    const std::filesystem::path& path);

}  // namespace arena::platform
