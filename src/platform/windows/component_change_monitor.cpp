#include "arena/ports/component_change_monitor.hpp"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <array>
#include <cstddef>
#include <stop_token>
#include <thread>
#include <utility>

namespace arena::ports {
namespace {

class WindowsComponentChangeMonitor final : public ComponentChangeMonitor {
  public:
    WindowsComponentChangeMonitor(HANDLE directory, ComponentChangeCallback callback)
        : directory_(directory), callback_(std::move(callback)),
          worker_([this](const std::stop_token stop_token) { run(stop_token); }) {}

    ~WindowsComponentChangeMonitor() override {
        worker_.request_stop();
        if (directory_ != INVALID_HANDLE_VALUE) {
            CancelIoEx(directory_, nullptr);
        }
        if (worker_.joinable()) {
            worker_.join();
        }
        if (directory_ != INVALID_HANDLE_VALUE) {
            CloseHandle(directory_);
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
        alignas(DWORD) std::array<std::byte, 64U * 1024U> buffer{};
        while (!stop_token.stop_requested()) {
            DWORD bytes = 0;
            const BOOL ok = ReadDirectoryChangesW(
                directory_, buffer.data(), static_cast<DWORD>(buffer.size()), TRUE,
                FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME |
                    FILE_NOTIFY_CHANGE_SIZE | FILE_NOTIFY_CHANGE_LAST_WRITE |
                    FILE_NOTIFY_CHANGE_CREATION | FILE_NOTIFY_CHANGE_SECURITY,
                &bytes, nullptr, nullptr);
            if (!ok) {
                const DWORD error = GetLastError();
                if (stop_token.stop_requested() || error == ERROR_OPERATION_ABORTED) {
                    return;
                }
                publish({ComponentChangeKind::overflow, {}});
                return;
            }
            if (bytes == 0) {
                publish({ComponentChangeKind::overflow, {}});
                continue;
            }
            DWORD offset = 0;
            while (offset < bytes) {
                const auto* event =
                    reinterpret_cast<const FILE_NOTIFY_INFORMATION*>(buffer.data() + offset);
                const std::wstring filename(event->FileName,
                                            event->FileNameLength / sizeof(wchar_t));
                publish({
                    ComponentChangeKind::changed,
                    std::filesystem::path(filename),
                });
                if (event->NextEntryOffset == 0) {
                    break;
                }
                offset += event->NextEntryOffset;
            }
        }
    }

    HANDLE directory_{INVALID_HANDLE_VALUE};
    ComponentChangeCallback callback_;
    std::jthread worker_;
};

} // namespace

std::unique_ptr<ComponentChangeMonitor> watch_component_tree(const std::filesystem::path& root,
                                                             ComponentChangeCallback callback) {
    const HANDLE directory = CreateFileW(
        root.c_str(), FILE_LIST_DIRECTORY, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (directory == INVALID_HANDLE_VALUE) {
        return nullptr;
    }
    return std::make_unique<WindowsComponentChangeMonitor>(directory, std::move(callback));
}

} // namespace arena::ports
