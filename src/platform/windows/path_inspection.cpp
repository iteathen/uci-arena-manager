#include "arena/platform/path_inspection.hpp"

#include <windows.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <iomanip>
#include <sstream>
#include <string>

namespace arena::platform {
namespace {

class Handle final {
  public:
    explicit Handle(const HANDLE value) : value_(value) {}
    ~Handle() {
        if (value_ != INVALID_HANDLE_VALUE) {
            CloseHandle(value_);
        }
    }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    [[nodiscard]] HANDLE get() const noexcept { return value_; }

  private:
    HANDLE value_{INVALID_HANDLE_VALUE};
};

std::string file_identity(const FILE_ID_INFO& identity) {
    std::ostringstream output;
    output << std::hex << std::setfill('0') << std::setw(16)
           << identity.VolumeSerialNumber << ':';
    for (const unsigned char byte : identity.FileId.Identifier) {
        output << std::setw(2) << static_cast<unsigned int>(byte);
    }
    return output.str();
}

bool executable_extension(const std::filesystem::path& path) {
    std::string extension = path.extension().string();
    std::ranges::transform(
        extension, extension.begin(),
        [](const unsigned char character) {
            return static_cast<char>(std::tolower(character));
        });
    return extension == ".exe";
}

}  // namespace

PathObservation inspect_path_no_follow(const std::filesystem::path& path) {
    const Handle handle(CreateFileW(
        path.c_str(), FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
        OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
    if (handle.get() == INVALID_HANDLE_VALUE) {
        const DWORD error = GetLastError();
        return {
            error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND
                ? PathEntryKind::missing
                : PathEntryKind::inaccessible,
            0,
            false,
            {},
            error,
        };
    }

    FILE_ATTRIBUTE_TAG_INFO attributes{};
    if (!GetFileInformationByHandleEx(
            handle.get(), FileAttributeTagInfo, &attributes,
            sizeof(attributes))) {
        return {
            PathEntryKind::inaccessible, 0, false, {}, GetLastError()};
    }
    if ((attributes.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0U) {
        return {PathEntryKind::link_or_reparse, 0, false, {}, 0};
    }

    FILE_STANDARD_INFO standard{};
    if (!GetFileInformationByHandleEx(
            handle.get(), FileStandardInfo, &standard, sizeof(standard))) {
        return {
            PathEntryKind::inaccessible, 0, false, {}, GetLastError()};
    }
    if (standard.Directory != FALSE) {
        return {PathEntryKind::directory, 0, false, {}, 0};
    }

    FILE_ID_INFO identity{};
    if (!GetFileInformationByHandleEx(
            handle.get(), FileIdInfo, &identity, sizeof(identity))) {
        return {
            PathEntryKind::inaccessible, 0, false, {}, GetLastError()};
    }
    return {
        PathEntryKind::regular_file,
        static_cast<std::uintmax_t>(standard.EndOfFile.QuadPart),
        executable_extension(path),
        file_identity(identity),
        0,
    };
}

}  // namespace arena::platform
