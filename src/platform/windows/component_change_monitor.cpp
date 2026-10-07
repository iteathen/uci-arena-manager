#include "arena/ports/component_change_monitor.hpp"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <array>
#include <cstddef>
#include <mutex>
#include <stop_token>
#include <thread>
#include <utility>

namespace arena::ports {
namespace {

class WindowsComponentChangeMonitor final : public ComponentChangeMonitor {
  public:
    WindowsComponentChangeMonitor(HANDLE directory, HANDLE event,
                                  ComponentChangeCallback callback)
        : directory_(directory), event_(event), callback_(std::move(callback)) {}

    bool start() {
        // Admission may trust verified bytes as soon as the factory returns.
        // Arm native notification buffering before exposing that authority.
        if (!arm()) return false;
        worker_ = std::jthread(
            [this](const std::stop_token stop_token) { run(stop_token); });
        return true;
    }

    ~WindowsComponentChangeMonitor() override {
        worker_.request_stop();
        {
            std::lock_guard lock(submission_mutex_);
            stopping_ = true;
            CancelIoEx(directory_, &operation_);
        }
        if (worker_.joinable()) {
            worker_.join();
        }
        // Cancellation is a request, not completion. Keep the OVERLAPPED and
        // buffer alive until the final operation has returned, even if thread
        // creation failed after initial arming.
        if (pending_) {
            DWORD bytes = 0;
            GetOverlappedResult(directory_, &operation_, &bytes, TRUE);
        }
        CloseHandle(event_);
        if (directory_ != INVALID_HANDLE_VALUE) {
            CloseHandle(directory_);
        }
    }

  private:
    bool arm() {
        std::lock_guard lock(submission_mutex_);
        if (stopping_) return false;
        pending_ = false;
        operation_ = {};
        operation_.hEvent = event_;
        ResetEvent(event_);
        pending_ = ReadDirectoryChangesW(
            directory_, buffer_.data(), static_cast<DWORD>(buffer_.size()), TRUE,
            FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME |
                FILE_NOTIFY_CHANGE_SIZE | FILE_NOTIFY_CHANGE_LAST_WRITE |
                FILE_NOTIFY_CHANGE_CREATION | FILE_NOTIFY_CHANGE_SECURITY,
            nullptr, &operation_, nullptr) != FALSE;
        return pending_;
    }

    void publish(const ComponentChange& change) const noexcept {
        try {
            callback_(change);
        } catch (...) {
        }
    }

    void run(const std::stop_token stop_token) {
        while (!stop_token.stop_requested()) {
            DWORD bytes = 0;
            const BOOL ok = GetOverlappedResult(directory_, &operation_, &bytes, TRUE);
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
            }
            DWORD offset = 0;
            while (offset < bytes) {
                const auto* event =
                    reinterpret_cast<const FILE_NOTIFY_INFORMATION*>(buffer_.data() + offset);
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
            if (!arm()) {
                if (!stop_token.stop_requested()) {
                    publish({ComponentChangeKind::overflow, {}});
                }
                return;
            }
        }
    }

    HANDLE directory_{INVALID_HANDLE_VALUE};
    HANDLE event_{nullptr};
    ComponentChangeCallback callback_;
    alignas(DWORD) std::array<std::byte, 64U * 1024U> buffer_{};
    OVERLAPPED operation_{};
    std::mutex submission_mutex_;
    bool pending_{false};
    bool stopping_{false};
    std::jthread worker_;
};

} // namespace

std::unique_ptr<ComponentChangeMonitor> watch_component_tree(const std::filesystem::path& root,
                                                             ComponentChangeCallback callback) {
    const HANDLE directory = CreateFileW(
        root.c_str(), FILE_LIST_DIRECTORY, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_OVERLAPPED,
        nullptr);
    if (directory == INVALID_HANDLE_VALUE) {
        return nullptr;
    }
    const HANDLE event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (event == nullptr) {
        CloseHandle(directory);
        return nullptr;
    }
    std::unique_ptr<WindowsComponentChangeMonitor> monitor;
    try {
        monitor = std::make_unique<WindowsComponentChangeMonitor>(
            directory, event, std::move(callback));
    } catch (...) {
        CloseHandle(event);
        CloseHandle(directory);
        return nullptr;
    }
    try {
        if (!monitor->start()) return nullptr;
    } catch (...) {
        return nullptr;
    }
    return monitor;
}

} // namespace arena::ports
