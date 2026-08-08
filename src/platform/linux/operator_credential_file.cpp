#include "arena/platform/operator_credential_file.hpp"

#include <sodium.h>

#include <array>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <pwd.h>
#include <stdexcept>
#include <string>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <vector>

namespace arena::platform {
namespace {

[[noreturn]] void throw_posix_error(const std::string& operation) {
    throw std::runtime_error(
        operation + " failed: " + std::string(std::strerror(errno)));
}

std::string random_suffix() {
    if (sodium_init() < 0) {
        throw std::runtime_error("credential transaction random source failed");
    }
    std::array<unsigned char, 16> bytes{};
    randombytes_buf(bytes.data(), bytes.size());
    std::array<char, 33> encoded{};
    sodium_bin2hex(encoded.data(), encoded.size(), bytes.data(), bytes.size());
    return encoded.data();
}

void require_private_directory(const std::filesystem::path& directory) {
    std::filesystem::create_directories(directory);
    struct stat state {};
    if (lstat(directory.c_str(), &state) != 0 || !S_ISDIR(state.st_mode) ||
        S_ISLNK(state.st_mode)) {
        throw std::runtime_error(
            "operator credential directory is not a private regular directory");
    }
    if (chmod(directory.c_str(), S_IRWXU) != 0) {
        throw_posix_error("Protect operator credential directory");
    }
}

bool regular_non_link_file(const std::filesystem::path& path) {
    struct stat state {};
    if (lstat(path.c_str(), &state) != 0) {
        if (errno == ENOENT) {
            return false;
        }
        throw_posix_error("Inspect operator credential file");
    }
    if (!S_ISREG(state.st_mode) || S_ISLNK(state.st_mode)) {
        throw std::runtime_error(
            "operator credential destination is not a regular file");
    }
    return true;
}

void write_private_file(
    const std::filesystem::path& path,
    const std::string_view contents) {
    const int file = open(
        path.c_str(),
        O_CREAT | O_EXCL | O_WRONLY | O_CLOEXEC | O_NOFOLLOW,
        S_IRUSR | S_IWUSR);
    if (file < 0) {
        throw_posix_error("Create private operator credential file");
    }
    std::size_t offset = 0;
    while (offset < contents.size()) {
        const ssize_t written =
            write(file, contents.data() + offset, contents.size() - offset);
        if (written < 0 && errno == EINTR) {
            continue;
        }
        if (written <= 0) {
            const int error = errno;
            close(file);
            unlink(path.c_str());
            errno = error;
            throw_posix_error("Write private operator credential file");
        }
        offset += static_cast<std::size_t>(written);
    }
    if (fsync(file) != 0) {
        const int error = errno;
        close(file);
        unlink(path.c_str());
        errno = error;
        throw_posix_error("Flush private operator credential file");
    }
    if (close(file) != 0) {
        unlink(path.c_str());
        throw_posix_error("Close private operator credential file");
    }
}

void sync_directory(const std::filesystem::path& directory) noexcept {
    const int file = open(directory.c_str(), O_RDONLY | O_CLOEXEC | O_DIRECTORY);
    if (file >= 0) {
        static_cast<void>(fsync(file));
        static_cast<void>(close(file));
    }
}

}  // namespace

std::filesystem::path default_operator_credential_root() {
    if (const char* configured = std::getenv("XDG_CONFIG_HOME");
        configured != nullptr && configured[0] != '\0' &&
        std::filesystem::path(configured).is_absolute()) {
        return std::filesystem::path(configured) / "uci-arena" / "credentials";
    }
    const long suggested = sysconf(_SC_GETPW_R_SIZE_MAX);
    std::vector<char> buffer(
        suggested > 0 ? static_cast<std::size_t>(suggested) : 16 * 1024);
    passwd entry{};
    passwd* result = nullptr;
    if (getpwuid_r(
            geteuid(), &entry, buffer.data(), buffer.size(), &result) != 0 ||
        result == nullptr || entry.pw_dir == nullptr || entry.pw_dir[0] == '\0') {
        throw std::runtime_error(
            "the interactive user's configuration directory is unavailable");
    }
    return std::filesystem::path(entry.pw_dir) / ".config" / "uci-arena" /
           "credentials";
}

OperatorCredentialFileUpdate::OperatorCredentialFileUpdate(
    std::filesystem::path destination,
    const std::string_view contents)
    : destination_(std::move(destination)) {
    if (destination_.empty() || destination_.filename().empty() ||
        contents.empty()) {
        throw std::invalid_argument(
            "operator credential update requires a destination and value");
    }
    try {
        require_private_directory(destination_.parent_path());
        const std::string suffix = random_suffix();
        staged_ = destination_.parent_path() /
                  (destination_.filename().string() + ".stage." + suffix);
        backup_ = destination_.parent_path() /
                  (destination_.filename().string() + ".backup." + suffix);
        write_private_file(staged_, contents);
        had_previous_ = regular_non_link_file(destination_);
        if (had_previous_ &&
            rename(destination_.c_str(), backup_.c_str()) != 0) {
            throw_posix_error("Stage preceding operator credential file");
        }
        if (rename(staged_.c_str(), destination_.c_str()) != 0) {
            throw_posix_error("Commit staged operator credential file");
        }
        active_ = true;
        if (chmod(destination_.c_str(), S_IRUSR | S_IWUSR) != 0) {
            throw_posix_error("Protect operator credential file");
        }
        sync_directory(destination_.parent_path());
    } catch (...) {
        rollback();
        throw;
    }
}

OperatorCredentialFileUpdate::~OperatorCredentialFileUpdate() {
    rollback();
}

const std::filesystem::path& OperatorCredentialFileUpdate::path() const noexcept {
    return destination_;
}

void OperatorCredentialFileUpdate::commit() {
    if (!active_) {
        throw std::logic_error("operator credential transaction is not active");
    }
    if (had_previous_ && unlink(backup_.c_str()) != 0) {
        rollback();
        throw_posix_error("Remove preceding operator credential file");
    }
    had_previous_ = false;
    backup_.clear();
    sync_directory(destination_.parent_path());
    active_ = false;
}

void OperatorCredentialFileUpdate::rollback() noexcept {
    if (!staged_.empty()) {
        static_cast<void>(unlink(staged_.c_str()));
    }
    if (active_) {
        static_cast<void>(unlink(destination_.c_str()));
    }
    if (had_previous_ && !backup_.empty()) {
        static_cast<void>(rename(backup_.c_str(), destination_.c_str()));
    }
    if (!destination_.empty() && !destination_.parent_path().empty()) {
        sync_directory(destination_.parent_path());
    }
    active_ = false;
}

}  // namespace arena::platform
