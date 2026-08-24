#include "arena/ports/component_change_monitor.hpp"

#include <poll.h>
#include <sys/inotify.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <stop_token>
#include <thread>
#include <unordered_map>
#include <utility>

namespace arena::ports {
namespace {

constexpr std::uint32_t watch_mask = IN_ATTRIB | IN_CLOSE_WRITE | IN_CREATE | IN_DELETE |
                                     IN_DELETE_SELF | IN_MODIFY | IN_MOVE_SELF | IN_MOVED_FROM |
                                     IN_MOVED_TO | IN_UNMOUNT;

class LinuxComponentChangeMonitor final : public ComponentChangeMonitor {
  public:
    LinuxComponentChangeMonitor(const int descriptor, std::filesystem::path root,
                                std::unordered_map<int, std::filesystem::path> directories,
                                ComponentChangeCallback callback)
        : descriptor_(descriptor), root_(std::move(root)), directories_(std::move(directories)),
          callback_(std::move(callback)),
          worker_([this](const std::stop_token stop_token) { run(stop_token); }) {}

    ~LinuxComponentChangeMonitor() override {
        worker_.request_stop();
        if (worker_.joinable()) {
            worker_.join();
        }
        if (descriptor_ >= 0) {
            close(descriptor_);
        }
    }

  private:
    void publish(const ComponentChange& change) const noexcept {
        try {
            callback_(change);
        } catch (...) {
        }
    }

    void run(const std::stop_token stop_token) {
        alignas(inotify_event) std::array<std::byte, 64U * 1024U> buffer{};
        while (!stop_token.stop_requested()) {
            pollfd descriptor{descriptor_, POLLIN, 0};
            const int ready = poll(&descriptor, 1, 100);
            if (ready < 0) {
                if (errno == EINTR) {
                    continue;
                }
                publish({ComponentChangeKind::overflow, {}});
                return;
            }
            if (ready == 0) {
                continue;
            }
            const ssize_t count = read(descriptor_, buffer.data(), buffer.size());
            if (count < 0) {
                if (errno == EAGAIN || errno == EINTR) {
                    continue;
                }
                publish({ComponentChangeKind::overflow, {}});
                return;
            }
            std::size_t offset = 0;
            while (offset < static_cast<std::size_t>(count)) {
                const auto* event = reinterpret_cast<const inotify_event*>(buffer.data() + offset);
                if ((event->mask & (IN_Q_OVERFLOW | IN_DELETE_SELF | IN_MOVE_SELF | IN_IGNORED |
                                    IN_UNMOUNT)) != 0U) {
                    publish({ComponentChangeKind::overflow, {}});
                } else if (const auto directory = directories_.find(event->wd);
                           directory != directories_.end()) {
                    std::filesystem::path path = directory->second;
                    if (event->len > 0 && event->name[0] != '\0') {
                        path /= event->name;
                    }
                    publish({
                        ComponentChangeKind::changed,
                        path.lexically_relative(root_),
                    });
                }
                offset += sizeof(inotify_event) + event->len;
            }
        }
    }

    int descriptor_{-1};
    std::filesystem::path root_;
    std::unordered_map<int, std::filesystem::path> directories_;
    ComponentChangeCallback callback_;
    std::jthread worker_;
};

bool add_directory_watch(const int descriptor, const std::filesystem::path& directory,
                         std::unordered_map<int, std::filesystem::path>& directories) {
    const int watch =
        inotify_add_watch(descriptor, directory.c_str(), watch_mask | IN_ONLYDIR | IN_DONT_FOLLOW);
    if (watch < 0) {
        return false;
    }
    directories.emplace(watch, directory);
    return true;
}

} // namespace

std::unique_ptr<ComponentChangeMonitor> watch_component_tree(const std::filesystem::path& root,
                                                             ComponentChangeCallback callback) {
    const int descriptor = inotify_init1(IN_CLOEXEC | IN_NONBLOCK);
    if (descriptor < 0) {
        return nullptr;
    }
    std::unordered_map<int, std::filesystem::path> directories;
    std::error_code error;
    if (!add_directory_watch(descriptor, root, directories)) {
        close(descriptor);
        return nullptr;
    }
    for (std::filesystem::recursive_directory_iterator
             iterator(root, std::filesystem::directory_options::skip_permission_denied, error),
         end;
         iterator != end; iterator.increment(error)) {
        if (error) {
            close(descriptor);
            return nullptr;
        }
        if (iterator->is_symlink(error)) {
            iterator.disable_recursion_pending();
            error.clear();
            continue;
        }
        if (iterator->is_directory(error) && !error &&
            !add_directory_watch(descriptor, iterator->path(), directories)) {
            close(descriptor);
            return nullptr;
        }
        error.clear();
    }
    return std::make_unique<LinuxComponentChangeMonitor>(descriptor, root, std::move(directories),
                                                         std::move(callback));
}

} // namespace arena::ports
