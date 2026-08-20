#include "arena/platform/client_key_store.hpp"

#include <sodium.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <iomanip>
#include <memory>
#include <sstream>
#include <string>
#include <string_view>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <vector>

namespace arena::platform {
namespace {

constexpr std::size_t key_payload_bytes = 64;
constexpr std::size_t key_file_bytes = key_payload_bytes + 1;
constexpr std::size_t key_read_bytes = key_file_bytes + 1;

using ports::ClientKeyFailure;
using ports::ClientKeyFailureKind;
using ports::ClientKeyMaterial;

[[noreturn]] void fail(const ClientKeyFailureKind kind) {
    throw ClientKeyFailure(kind);
}

bool valid_key(const std::string_view value) {
    return value.size() == key_payload_bytes &&
           std::ranges::all_of(value, [](const unsigned char character) {
               return std::isdigit(character) != 0 ||
                      (character >= 'a' && character <= 'f');
           });
}

class Descriptor final {
  public:
    explicit Descriptor(const int value = -1) : value_(value) {}
    ~Descriptor() {
        if (value_ >= 0) close(value_);
    }
    Descriptor(const Descriptor&) = delete;
    Descriptor& operator=(const Descriptor&) = delete;
    Descriptor(Descriptor&& other) noexcept : value_(other.value_) {
        other.value_ = -1;
    }
    Descriptor& operator=(Descriptor&& other) noexcept {
        if (this != &other) {
            if (value_ >= 0) close(value_);
            value_ = other.value_;
            other.value_ = -1;
        }
        return *this;
    }
    [[nodiscard]] int get() const noexcept { return value_; }
    [[nodiscard]] bool close_checked() noexcept {
        if (value_ < 0) return true;
        const int value = value_;
        value_ = -1;
        return close(value) == 0;
    }

  private:
    int value_;
};

bool same_generation(const struct stat& left, const struct stat& right) {
    return left.st_dev == right.st_dev && left.st_ino == right.st_ino &&
           left.st_size == right.st_size &&
           left.st_mtim.tv_sec == right.st_mtim.tv_sec &&
           left.st_mtim.tv_nsec == right.st_mtim.tv_nsec &&
           left.st_ctim.tv_sec == right.st_ctim.tv_sec &&
           left.st_ctim.tv_nsec == right.st_ctim.tv_nsec;
}

void require_directory(
    const int descriptor,
    const bool restrict_writes,
    struct stat* observed = nullptr) {
    struct stat state {};
    if (fstat(descriptor, &state) != 0) fail(ClientKeyFailureKind::io);
    if (!S_ISDIR(state.st_mode)) fail(ClientKeyFailureKind::unsafe);
    if (restrict_writes &&
        (state.st_mode & (S_IWGRP | S_IWOTH)) != 0) {
        fail(ClientKeyFailureKind::unsafe);
    }
    if (observed != nullptr) *observed = state;
}

struct RootAuthority final {
    std::vector<Descriptor> pinned_ancestors;
    struct stat root_state {};

    [[nodiscard]] int root() const noexcept {
        return pinned_ancestors.back().get();
    }
};

RootAuthority open_root(
    const std::filesystem::path& root,
    const bool create_final) {
    RootAuthority authority;
    Descriptor volume(open("/", O_RDONLY | O_CLOEXEC | O_DIRECTORY));
    if (volume.get() < 0) fail(ClientKeyFailureKind::io);
    require_directory(volume.get(), false);
    authority.pinned_ancestors.push_back(std::move(volume));

    std::filesystem::path current = root.root_path();
    for (const auto& component : root.relative_path()) {
        current /= component;
        const std::string name = component.string();
        int descriptor = openat(
            authority.pinned_ancestors.back().get(),
            name.c_str(),
            O_RDONLY | O_CLOEXEC | O_DIRECTORY | O_NOFOLLOW);
        if (descriptor < 0 && errno == ENOENT && current == root && create_final) {
            if (mkdirat(
                    authority.pinned_ancestors.back().get(),
                    name.c_str(),
                    S_IRWXU) != 0 &&
                errno != EEXIST) {
                fail(ClientKeyFailureKind::io);
            }
            descriptor = openat(
                authority.pinned_ancestors.back().get(),
                name.c_str(),
                O_RDONLY | O_CLOEXEC | O_DIRECTORY | O_NOFOLLOW);
        }
        if (descriptor < 0) {
            const int error = errno;
            fail(
                error == ENOENT
                    ? ClientKeyFailureKind::missing
                    : error == ELOOP || error == ENOTDIR
                          ? ClientKeyFailureKind::unsafe
                          : ClientKeyFailureKind::io);
        }
        Descriptor directory(descriptor);
        require_directory(
            directory.get(),
            current == root,
            current == root ? &authority.root_state : nullptr);
        authority.pinned_ancestors.push_back(std::move(directory));
    }
    return authority;
}

std::string identity_string(const struct stat& state) {
    std::ostringstream output;
    output << std::hex << std::setfill('0')
           << static_cast<std::uintmax_t>(state.st_dev) << ':'
           << static_cast<std::uintmax_t>(state.st_ino) << ':'
           << static_cast<std::uintmax_t>(state.st_ctim.tv_sec) << ':'
           << static_cast<std::uintmax_t>(state.st_ctim.tv_nsec);
    return output.str();
}

ClientKeyMaterial load_key(const RootAuthority& authority) {
    Descriptor file(openat(
        authority.root(),
        "client.key",
        O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK));
    if (file.get() < 0) {
        const int error = errno;
        fail(
            error == ENOENT
                ? ClientKeyFailureKind::missing
                : error == ELOOP || error == ENOTDIR
                      ? ClientKeyFailureKind::unsafe
                      : ClientKeyFailureKind::io);
    }

    struct stat before {};
    if (fstat(file.get(), &before) != 0) fail(ClientKeyFailureKind::io);
    constexpr mode_t permitted = S_IRUSR | S_IWUSR | S_IRGRP;
    if (!S_ISREG(before.st_mode) || before.st_uid != authority.root_state.st_uid) {
        fail(ClientKeyFailureKind::unsafe);
    }
    if ((before.st_mode & (S_IRUSR | S_IWUSR)) !=
            (S_IRUSR | S_IWUSR) ||
        (before.st_mode & 07777 & ~permitted) != 0) {
        fail(ClientKeyFailureKind::unsafe);
    }
    if (before.st_size != static_cast<off_t>(key_file_bytes)) {
        fail(ClientKeyFailureKind::invalid);
    }

    std::array<char, key_read_bytes> bytes{};
    std::size_t total = 0;
    while (total < bytes.size()) {
        const ssize_t observed =
            read(file.get(), bytes.data() + total, bytes.size() - total);
        if (observed < 0 && errno == EINTR) continue;
        if (observed < 0) fail(ClientKeyFailureKind::io);
        if (observed == 0) break;
        total += static_cast<std::size_t>(observed);
    }

    struct stat after {};
    struct stat path_after {};
    if (fstat(file.get(), &after) != 0 ||
        fstatat(
            authority.root(), "client.key", &path_after,
            AT_SYMLINK_NOFOLLOW) != 0) {
        fail(ClientKeyFailureKind::io);
    }
    if (!same_generation(before, after) ||
        !same_generation(before, path_after) ||
        total != key_file_bytes || bytes[key_payload_bytes] != '\n') {
        fail(ClientKeyFailureKind::invalid);
    }
    std::string value(bytes.data(), key_payload_bytes);
    if (!valid_key(value)) fail(ClientKeyFailureKind::invalid);
    const std::string identity = identity_string(before);
    if (!file.close_checked()) fail(ClientKeyFailureKind::io);
    return {std::move(value), identity};
}

std::string random_suffix() {
    if (sodium_init() < 0) fail(ClientKeyFailureKind::io);
    std::array<unsigned char, 16> bytes{};
    randombytes_buf(bytes.data(), bytes.size());
    std::array<char, 33> encoded{};
    sodium_bin2hex(encoded.data(), encoded.size(), bytes.data(), bytes.size());
    return encoded.data();
}

class LinuxClientKeyStore final : public ports::ClientKeyStore {
  public:
    explicit LinuxClientKeyStore(const std::filesystem::path& state_directory) {
        try {
            root_ = std::filesystem::absolute(state_directory).lexically_normal();
        } catch (const std::filesystem::filesystem_error&) {
            fail(ClientKeyFailureKind::unsafe);
        }
        if (root_.empty() || !root_.is_absolute() ||
            root_ == root_.root_path() || root_.native().size() > 4096) {
            fail(ClientKeyFailureKind::unsafe);
        }
    }

    [[nodiscard]] ClientKeyMaterial load_existing() const override {
        const RootAuthority authority = open_root(root_, false);
        return load_key(authority);
    }

    [[nodiscard]] ClientKeyMaterial load_or_create(
        const std::string_view proposed_key) override {
        if (!valid_key(proposed_key)) fail(ClientKeyFailureKind::invalid);
        const RootAuthority authority = open_root(root_, true);
        try {
            return load_key(authority);
        } catch (const ClientKeyFailure& error) {
            if (error.kind() != ClientKeyFailureKind::missing) throw;
        }

        const std::string staged = ".client.key.stage." + random_suffix();
        Descriptor file(openat(
            authority.root(),
            staged.c_str(),
            O_CREAT | O_EXCL | O_WRONLY | O_CLOEXEC | O_NOFOLLOW,
            S_IRUSR | S_IWUSR));
        if (file.get() < 0 ||
            fchmod(file.get(), S_IRUSR | S_IWUSR) != 0) {
            if (file.get() >= 0) unlinkat(authority.root(), staged.c_str(), 0);
            fail(ClientKeyFailureKind::io);
        }

        const std::string serialized = std::string(proposed_key) + '\n';
        std::size_t offset = 0;
        while (offset < serialized.size()) {
            const ssize_t written = write(
                file.get(),
                serialized.data() + offset,
                serialized.size() - offset);
            if (written < 0 && errno == EINTR) continue;
            if (written <= 0) {
                static_cast<void>(file.close_checked());
                unlinkat(authority.root(), staged.c_str(), 0);
                fail(ClientKeyFailureKind::io);
            }
            offset += static_cast<std::size_t>(written);
        }
        if (fsync(file.get()) != 0 || !file.close_checked()) {
            unlinkat(authority.root(), staged.c_str(), 0);
            fail(ClientKeyFailureKind::io);
        }

        if (linkat(
                authority.root(), staged.c_str(),
                authority.root(), "client.key", 0) != 0) {
            const int error = errno;
            static_cast<void>(unlinkat(authority.root(), staged.c_str(), 0));
            if (error != EEXIST) fail(ClientKeyFailureKind::io);
            return load_key(authority);
        }
        if (unlinkat(authority.root(), staged.c_str(), 0) != 0 ||
            fsync(authority.root()) != 0) {
            fail(ClientKeyFailureKind::io);
        }
        return load_key(authority);
    }

  private:
    std::filesystem::path root_;
};

}  // namespace

std::shared_ptr<ports::ClientKeyStore> make_client_key_store(
    const std::filesystem::path& state_directory) {
    return std::make_shared<LinuxClientKeyStore>(state_directory);
}

}  // namespace arena::platform
