#include "arena/platform/operator_credential_file.hpp"

#define NOMINMAX
#include <aclapi.h>
#include <shlobj.h>
#include <sodium.h>
#include <windows.h>

#include <array>
#include <algorithm>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

namespace arena::platform {
namespace {

[[noreturn]] void throw_windows_error(
    const std::string& operation,
    const DWORD error = GetLastError()) {
    throw std::runtime_error(
        operation + " failed with Windows error " + std::to_string(error));
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

class PrivateSecurity final {
  public:
    PrivateSecurity() {
        HANDLE token = nullptr;
        if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) {
            throw_windows_error("OpenProcessToken");
        }
        DWORD required = 0;
        GetTokenInformation(token, TokenUser, nullptr, 0, &required);
        token_user_.resize(required);
        if (required == 0 ||
            !GetTokenInformation(
                token, TokenUser, token_user_.data(), required, &required)) {
            const DWORD error = GetLastError();
            CloseHandle(token);
            throw_windows_error("GetTokenInformation", error);
        }
        CloseHandle(token);

        user_sid_ = reinterpret_cast<TOKEN_USER*>(token_user_.data())->User.Sid;
        DWORD system_sid_size = static_cast<DWORD>(system_sid_.size());
        if (!CreateWellKnownSid(
                WinLocalSystemSid,
                nullptr,
                system_sid_.data(),
                &system_sid_size)) {
            throw_windows_error("CreateWellKnownSid");
        }

        std::array<EXPLICIT_ACCESSW, 2> access{};
        initialize_access(access[0], user_sid_);
        initialize_access(access[1], system_sid_.data());
        const DWORD acl_result =
            SetEntriesInAclW(
                static_cast<ULONG>(access.size()), access.data(), nullptr, &acl_);
        if (acl_result != ERROR_SUCCESS) {
            throw_windows_error("SetEntriesInAclW", acl_result);
        }
        if (!InitializeSecurityDescriptor(
                &descriptor_, SECURITY_DESCRIPTOR_REVISION) ||
            !SetSecurityDescriptorOwner(&descriptor_, user_sid_, FALSE) ||
            !SetSecurityDescriptorDacl(&descriptor_, TRUE, acl_, FALSE) ||
            !SetSecurityDescriptorControl(
                &descriptor_, SE_DACL_PROTECTED, SE_DACL_PROTECTED)) {
            throw_windows_error("Initialize private security descriptor");
        }
        attributes_.nLength = sizeof(attributes_);
        attributes_.lpSecurityDescriptor = &descriptor_;
        attributes_.bInheritHandle = FALSE;
    }

    ~PrivateSecurity() {
        if (acl_ != nullptr) {
            LocalFree(acl_);
        }
    }

    [[nodiscard]] SECURITY_ATTRIBUTES* attributes() noexcept {
        return &attributes_;
    }

    void protect(const std::filesystem::path& path) const {
        std::wstring value = path.native();
        const DWORD result = SetNamedSecurityInfoW(
            value.data(),
            SE_FILE_OBJECT,
            OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION |
                PROTECTED_DACL_SECURITY_INFORMATION,
            user_sid_,
            nullptr,
            acl_,
            nullptr);
        if (result != ERROR_SUCCESS) {
            throw_windows_error("Protect operator credential path", result);
        }
    }

  private:
    static void initialize_access(EXPLICIT_ACCESSW& access, PSID sid) {
        access.grfAccessPermissions = FILE_ALL_ACCESS;
        access.grfAccessMode = SET_ACCESS;
        access.grfInheritance = SUB_CONTAINERS_AND_OBJECTS_INHERIT;
        access.Trustee.TrusteeForm = TRUSTEE_IS_SID;
        access.Trustee.TrusteeType = TRUSTEE_IS_USER;
        access.Trustee.ptstrName = reinterpret_cast<LPWSTR>(sid);
    }

    std::vector<unsigned char> token_user_;
    std::array<unsigned char, SECURITY_MAX_SID_SIZE> system_sid_{};
    PSID user_sid_{};
    PACL acl_{};
    SECURITY_DESCRIPTOR descriptor_{};
    SECURITY_ATTRIBUTES attributes_{};
};

void require_private_directory(
    const std::filesystem::path& directory,
    PrivateSecurity& security) {
    std::filesystem::create_directories(directory.parent_path());
    if (!CreateDirectoryW(directory.c_str(), security.attributes())) {
        const DWORD error = GetLastError();
        if (error != ERROR_ALREADY_EXISTS) {
            throw_windows_error("Create operator credential directory", error);
        }
    }
    const DWORD attributes = GetFileAttributesW(directory.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES ||
        (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0 ||
        (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
        throw std::runtime_error(
            "operator credential directory is not a private regular directory");
    }
    security.protect(directory);
}

bool regular_non_reparse_file(const std::filesystem::path& path) {
    const DWORD attributes = GetFileAttributesW(path.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES) {
        if (GetLastError() == ERROR_FILE_NOT_FOUND ||
            GetLastError() == ERROR_PATH_NOT_FOUND) {
            return false;
        }
        throw_windows_error("Inspect operator credential file");
    }
    if ((attributes & FILE_ATTRIBUTE_DIRECTORY) != 0 ||
        (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
        throw std::runtime_error(
            "operator credential destination is not a regular file");
    }
    return true;
}

void write_private_file(
    const std::filesystem::path& path,
    const std::string_view contents,
    PrivateSecurity& security) {
    HANDLE file = CreateFileW(
        path.c_str(),
        GENERIC_WRITE,
        0,
        security.attributes(),
        CREATE_NEW,
        FILE_ATTRIBUTE_TEMPORARY | FILE_ATTRIBUTE_NOT_CONTENT_INDEXED,
        nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        throw_windows_error("Create private operator credential file");
    }
    std::size_t offset = 0;
    while (offset < contents.size()) {
        const DWORD requested = static_cast<DWORD>(std::min<std::size_t>(
            contents.size() - offset,
            std::numeric_limits<DWORD>::max()));
        DWORD written = 0;
        if (!WriteFile(
                file, contents.data() + offset, requested, &written, nullptr) ||
            written == 0) {
            const DWORD error = GetLastError();
            CloseHandle(file);
            DeleteFileW(path.c_str());
            throw_windows_error("Write private operator credential file", error);
        }
        offset += written;
    }
    if (!FlushFileBuffers(file)) {
        const DWORD error = GetLastError();
        CloseHandle(file);
        DeleteFileW(path.c_str());
        throw_windows_error("Flush private operator credential file", error);
    }
    CloseHandle(file);
    security.protect(path);
}

}  // namespace

std::filesystem::path default_operator_credential_root() {
    PWSTR raw = nullptr;
    const HRESULT result = SHGetKnownFolderPath(
        FOLDERID_LocalAppData, KF_FLAG_DEFAULT, nullptr, &raw);
    if (FAILED(result) || raw == nullptr) {
        if (raw != nullptr) {
            CoTaskMemFree(raw);
        }
        throw std::runtime_error(
            "the interactive user's local application-data directory is unavailable");
    }
    const std::filesystem::path root =
        std::filesystem::path(raw) / "UCI Arena" / "Credentials";
    CoTaskMemFree(raw);
    return root;
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
        PrivateSecurity security;
        require_private_directory(destination_.parent_path(), security);
        const std::string suffix = random_suffix();
        staged_ = destination_.parent_path() /
                  (destination_.filename().string() + ".stage." + suffix);
        backup_ = destination_.parent_path() /
                  (destination_.filename().string() + ".backup." + suffix);
        write_private_file(staged_, contents, security);
        had_previous_ = regular_non_reparse_file(destination_);
        if (had_previous_ &&
            !MoveFileExW(
                destination_.c_str(),
                backup_.c_str(),
                MOVEFILE_WRITE_THROUGH)) {
            throw_windows_error("Stage preceding operator credential file");
        }
        if (!MoveFileExW(
                staged_.c_str(),
                destination_.c_str(),
                MOVEFILE_WRITE_THROUGH)) {
            throw_windows_error("Commit staged operator credential file");
        }
        active_ = true;
        security.protect(destination_);
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
    if (had_previous_ && !DeleteFileW(backup_.c_str())) {
        const DWORD error = GetLastError();
        rollback();
        throw_windows_error("Remove preceding operator credential file", error);
    }
    had_previous_ = false;
    backup_.clear();
    active_ = false;
}

void OperatorCredentialFileUpdate::rollback() noexcept {
    if (!staged_.empty()) {
        DeleteFileW(staged_.c_str());
    }
    if (active_) {
        DeleteFileW(destination_.c_str());
    }
    if (had_previous_ && !backup_.empty()) {
        MoveFileExW(
            backup_.c_str(),
            destination_.c_str(),
            MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
    }
    active_ = false;
}

}  // namespace arena::platform
