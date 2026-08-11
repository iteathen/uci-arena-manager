#include "arena/platform/path_inspection.hpp"

#include <sys/stat.h>

#include <cerrno>
#include <cstdint>
#include <iomanip>
#include <sstream>

namespace arena::platform {

PathObservation inspect_path_no_follow(const std::filesystem::path& path) {
    struct stat information {};
    if (::lstat(path.c_str(), &information) != 0) {
        const int error = errno;
        return {
            error == ENOENT || error == ENOTDIR
                ? PathEntryKind::missing
                : PathEntryKind::inaccessible,
            0,
            false,
            {},
            static_cast<std::uint32_t>(error),
        };
    }
    if (S_ISLNK(information.st_mode)) {
        return {PathEntryKind::link_or_reparse, 0, false, {}, 0};
    }
    if (S_ISDIR(information.st_mode)) {
        return {PathEntryKind::directory, 0, false, {}, 0};
    }
    if (!S_ISREG(information.st_mode)) {
        return {PathEntryKind::other, 0, false, {}, 0};
    }
    std::ostringstream identity;
    identity << std::hex << std::setfill('0')
             << static_cast<std::uintmax_t>(information.st_dev) << ':'
             << static_cast<std::uintmax_t>(information.st_ino);
    constexpr mode_t executable_bits = S_IXUSR | S_IXGRP | S_IXOTH;
    return {
        PathEntryKind::regular_file,
        static_cast<std::uintmax_t>(information.st_size),
        (information.st_mode & executable_bits) != 0,
        identity.str(),
        0,
    };
}

}  // namespace arena::platform
