#include "arena/platform/client_key_store.hpp"

#define NOMINMAX
#include <aclapi.h>
#include <sodium.h>
#include <windows.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <iomanip>
#include <memory>
#include <sstream>
#include <string>
#include <string_view>
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

class Handle final {
  public:
    explicit Handle(const HANDLE value = INVALID_HANDLE_VALUE) : value_(value) {}
    ~Handle() {
        if (value_ != INVALID_HANDLE_VALUE) CloseHandle(value_);
    }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    Handle(Handle&& other) noexcept : value_(other.value_) {
        other.value_ = INVALID_HANDLE_VALUE;
    }
    Handle& operator=(Handle&& other) noexcept {
        if (this != &other) {
            if (value_ != INVALID_HANDLE_VALUE) CloseHandle(value_);
            value_ = other.value_;
            other.value_ = INVALID_HANDLE_VALUE;
        }
        return *this;
    }
    [[nodiscard]] HANDLE get() const noexcept { return value_; }
    [[nodiscard]] bool close_checked() noexcept {
        if (value_ == INVALID_HANDLE_VALUE) return true;
        const HANDLE value = value_;
        value_ = INVALID_HANDLE_VALUE;
        return CloseHandle(value) != FALSE;
    }

  private:
    HANDLE value_;
};

bool broad_principal(const PSID sid) {
    return IsWellKnownSid(sid, WinWorldSid) ||
           IsWellKnownSid(sid, WinAuthenticatedUserSid) ||
           IsWellKnownSid(sid, WinBuiltinUsersSid) ||
           IsWellKnownSid(sid, WinAnonymousSid);
}

bool trusted_owner(const PSID owner) {
    if (IsWellKnownSid(owner, WinLocalSystemSid) ||
        IsWellKnownSid(owner, WinLocalServiceSid) ||
        IsWellKnownSid(owner, WinBuiltinAdministratorsSid)) {
        return true;
    }
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) {
        fail(ClientKeyFailureKind::io);
    }
    DWORD required = 0;
    GetTokenInformation(token, TokenUser, nullptr, 0, &required);
    std::vector<unsigned char> token_user(required);
    if (required == 0 ||
        !GetTokenInformation(
            token, TokenUser, token_user.data(), required, &required)) {
        CloseHandle(token);
        fail(ClientKeyFailureKind::io);
    }
    CloseHandle(token);
    return EqualSid(
               owner,
               reinterpret_cast<TOKEN_USER*>(token_user.data())->User.Sid) !=
           FALSE;
}

bool trusted_writer(const PSID sid, const PSID owner) {
    return EqualSid(sid, owner) != FALSE ||
           IsWellKnownSid(sid, WinLocalSystemSid) ||
           IsWellKnownSid(sid, WinLocalServiceSid) ||
           IsWellKnownSid(sid, WinBuiltinAdministratorsSid);
}

void require_restricted_security(const HANDLE handle) {
    PSID owner = nullptr;
    PACL dacl = nullptr;
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    const DWORD result = GetSecurityInfo(
        handle,
        SE_FILE_OBJECT,
        OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION,
        &owner,
        nullptr,
        &dacl,
        nullptr,
        &descriptor);
    if (result != ERROR_SUCCESS || descriptor == nullptr || owner == nullptr) {
        if (descriptor != nullptr) LocalFree(descriptor);
        fail(ClientKeyFailureKind::unsafe);
    }
    if (broad_principal(owner) || !trusted_owner(owner)) {
        LocalFree(descriptor);
        fail(ClientKeyFailureKind::unsafe);
    }
    BOOL dacl_present = FALSE;
    BOOL dacl_defaulted = FALSE;
    PACL observed_dacl = nullptr;
    if (!GetSecurityDescriptorDacl(
            descriptor, &dacl_present, &observed_dacl, &dacl_defaulted) ||
        dacl_present == FALSE || observed_dacl == nullptr || dacl == nullptr) {
        LocalFree(descriptor);
        fail(ClientKeyFailureKind::unsafe);
    }
    constexpr DWORD dangerous =
        FILE_WRITE_DATA | FILE_APPEND_DATA | FILE_WRITE_EA |
        FILE_WRITE_ATTRIBUTES | DELETE | WRITE_DAC | WRITE_OWNER |
        GENERIC_WRITE | GENERIC_ALL;
    for (DWORD index = 0; index < observed_dacl->AceCount; ++index) {
        void* raw_ace = nullptr;
        if (!GetAce(observed_dacl, index, &raw_ace) || raw_ace == nullptr) {
            LocalFree(descriptor);
            fail(ClientKeyFailureKind::unsafe);
        }
        const auto* header = static_cast<const ACE_HEADER*>(raw_ace);
        if (header->AceType == ACCESS_ALLOWED_OBJECT_ACE_TYPE ||
            header->AceType == ACCESS_ALLOWED_CALLBACK_ACE_TYPE ||
            header->AceType == ACCESS_ALLOWED_CALLBACK_OBJECT_ACE_TYPE) {
            LocalFree(descriptor);
            fail(ClientKeyFailureKind::unsafe);
        }
        if (header->AceType != ACCESS_ALLOWED_ACE_TYPE) continue;
        const auto* ace = static_cast<const ACCESS_ALLOWED_ACE*>(raw_ace);
        const PSID sid = const_cast<DWORD*>(&ace->SidStart);
        if ((ace->Mask & dangerous) != 0U && !trusted_writer(sid, owner)) {
            LocalFree(descriptor);
            fail(ClientKeyFailureKind::unsafe);
        }
    }
    LocalFree(descriptor);
}

void require_directory_handle(const HANDLE handle, const bool restrict_writes) {
    FILE_ATTRIBUTE_TAG_INFO attributes{};
    FILE_STANDARD_INFO standard{};
    if (!GetFileInformationByHandleEx(
            handle, FileAttributeTagInfo, &attributes, sizeof(attributes)) ||
        !GetFileInformationByHandleEx(
            handle, FileStandardInfo, &standard, sizeof(standard))) {
        fail(ClientKeyFailureKind::io);
    }
    if ((attributes.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0U ||
        standard.Directory == FALSE) {
        fail(ClientKeyFailureKind::unsafe);
    }
    if (restrict_writes) require_restricted_security(handle);
}

class PrivateObjectSecurity final {
  public:
    explicit PrivateObjectSecurity(const DWORD users_permissions) {
        HANDLE token = nullptr;
        if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) {
            fail(ClientKeyFailureKind::io);
        }
        DWORD required = 0;
        GetTokenInformation(token, TokenUser, nullptr, 0, &required);
        token_user_.resize(required);
        if (required == 0 ||
            !GetTokenInformation(
                token, TokenUser, token_user_.data(), required, &required)) {
            CloseHandle(token);
            fail(ClientKeyFailureKind::io);
        }
        CloseHandle(token);
        current_user_ =
            reinterpret_cast<TOKEN_USER*>(token_user_.data())->User.Sid;

        initialize_well_known(WinLocalSystemSid, system_sid_);
        initialize_well_known(WinBuiltinAdministratorsSid, administrators_sid_);
        initialize_well_known(WinBuiltinUsersSid, users_sid_);

        std::array<EXPLICIT_ACCESSW, 4> access{};
        initialize_access(access[0], current_user_, FILE_ALL_ACCESS);
        initialize_access(access[1], system_sid_.data(), FILE_ALL_ACCESS);
        initialize_access(
            access[2], administrators_sid_.data(), FILE_ALL_ACCESS);
        initialize_access(access[3], users_sid_.data(), users_permissions);
        const DWORD acl_result = SetEntriesInAclW(
            static_cast<ULONG>(access.size()), access.data(), nullptr, &acl_);
        if (acl_result != ERROR_SUCCESS) {
            fail(ClientKeyFailureKind::io);
        }
        if (!InitializeSecurityDescriptor(
                &descriptor_, SECURITY_DESCRIPTOR_REVISION) ||
            !SetSecurityDescriptorOwner(
                &descriptor_, current_user_, FALSE) ||
            !SetSecurityDescriptorDacl(
                &descriptor_, TRUE, acl_, FALSE) ||
            !SetSecurityDescriptorControl(
                &descriptor_, SE_DACL_PROTECTED, SE_DACL_PROTECTED)) {
            LocalFree(acl_);
            acl_ = nullptr;
            fail(ClientKeyFailureKind::io);
        }
        attributes_.nLength = sizeof(attributes_);
        attributes_.lpSecurityDescriptor = &descriptor_;
        attributes_.bInheritHandle = FALSE;
    }

    ~PrivateObjectSecurity() {
        if (acl_ != nullptr) LocalFree(acl_);
    }

    [[nodiscard]] SECURITY_ATTRIBUTES* attributes() noexcept {
        return &attributes_;
    }

  private:
    static void initialize_well_known(
        const WELL_KNOWN_SID_TYPE kind,
        std::array<unsigned char, SECURITY_MAX_SID_SIZE>& buffer) {
        DWORD size = static_cast<DWORD>(buffer.size());
        if (!CreateWellKnownSid(kind, nullptr, buffer.data(), &size)) {
            fail(ClientKeyFailureKind::io);
        }
    }

    static void initialize_access(
        EXPLICIT_ACCESSW& access,
        const PSID sid,
        const DWORD permissions) {
        access.grfAccessPermissions = permissions;
        access.grfAccessMode = SET_ACCESS;
        access.grfInheritance = NO_INHERITANCE;
        access.Trustee.TrusteeForm = TRUSTEE_IS_SID;
        access.Trustee.TrusteeType = TRUSTEE_IS_UNKNOWN;
        access.Trustee.ptstrName = reinterpret_cast<LPWSTR>(sid);
    }

    std::vector<unsigned char> token_user_;
    std::array<unsigned char, SECURITY_MAX_SID_SIZE> system_sid_{};
    std::array<unsigned char, SECURITY_MAX_SID_SIZE> administrators_sid_{};
    std::array<unsigned char, SECURITY_MAX_SID_SIZE> users_sid_{};
    PSID current_user_{};
    PACL acl_{};
    SECURITY_DESCRIPTOR descriptor_{};
    SECURITY_ATTRIBUTES attributes_{};
};

struct RootAuthority final {
    std::vector<Handle> pinned_ancestors;

    [[nodiscard]] HANDLE root() const noexcept {
        return pinned_ancestors.back().get();
    }
};

RootAuthority open_root(
    const std::filesystem::path& root,
    const bool create_final) {
    RootAuthority authority;
    std::filesystem::path current = root.root_path();
    const auto open_directory = [&](const std::filesystem::path& path) {
        return Handle(CreateFileW(
            path.c_str(),
            FILE_LIST_DIRECTORY | FILE_READ_ATTRIBUTES | READ_CONTROL,
            FILE_SHARE_READ | FILE_SHARE_WRITE,
            nullptr,
            OPEN_EXISTING,
            FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT,
            nullptr));
    };

    Handle volume = open_directory(current);
    if (volume.get() == INVALID_HANDLE_VALUE) fail(ClientKeyFailureKind::unsafe);
    require_directory_handle(volume.get(), false);
    authority.pinned_ancestors.push_back(std::move(volume));

    for (const auto& component : root.relative_path()) {
        current /= component;
        DWORD attributes = GetFileAttributesW(current.c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES) {
            const DWORD error = GetLastError();
            if (current != root || !create_final ||
                (error != ERROR_FILE_NOT_FOUND && error != ERROR_PATH_NOT_FOUND)) {
                fail(
                    error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND
                        ? ClientKeyFailureKind::missing
                        : ClientKeyFailureKind::io);
            }
            PrivateObjectSecurity security(
                FILE_GENERIC_READ | FILE_GENERIC_EXECUTE);
            if (!CreateDirectoryW(current.c_str(), security.attributes())) {
                const DWORD create_error = GetLastError();
                if (create_error != ERROR_ALREADY_EXISTS) {
                    fail(ClientKeyFailureKind::io);
                }
            }
            attributes = GetFileAttributesW(current.c_str());
        }
        if (attributes == INVALID_FILE_ATTRIBUTES ||
            (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0U ||
            (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0U) {
            fail(ClientKeyFailureKind::unsafe);
        }
        Handle directory = open_directory(current);
        if (directory.get() == INVALID_HANDLE_VALUE) {
            fail(ClientKeyFailureKind::io);
        }
        require_directory_handle(directory.get(), current == root);
        authority.pinned_ancestors.push_back(std::move(directory));
    }
    return authority;
}

std::string identity_string(
    const FILE_ID_INFO& identity,
    const FILE_BASIC_INFO& basic) {
    std::ostringstream output;
    output << std::hex << std::setfill('0') << std::setw(16)
           << identity.VolumeSerialNumber << ':';
    for (const unsigned char byte : identity.FileId.Identifier) {
        output << std::setw(2) << static_cast<unsigned int>(byte);
    }
    output << ':' << std::setw(16) << basic.ChangeTime.QuadPart;
    return output.str();
}

ClientKeyMaterial load_key(
    const std::filesystem::path& key_path,
    const RootAuthority&) {
    const DWORD attributes = GetFileAttributesW(key_path.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES) {
        const DWORD error = GetLastError();
        fail(
            error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND
                ? ClientKeyFailureKind::missing
                : ClientKeyFailureKind::io);
    }
    if ((attributes & FILE_ATTRIBUTE_DIRECTORY) != 0U ||
        (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0U) {
        fail(ClientKeyFailureKind::unsafe);
    }

    Handle file(CreateFileW(
        key_path.c_str(),
        GENERIC_READ | FILE_READ_ATTRIBUTES | READ_CONTROL,
        FILE_SHARE_READ,
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT |
            FILE_FLAG_SEQUENTIAL_SCAN,
        nullptr));
    if (file.get() == INVALID_HANDLE_VALUE) {
        const DWORD error = GetLastError();
        fail(
            error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND
                ? ClientKeyFailureKind::missing
                : ClientKeyFailureKind::io);
    }

    FILE_ATTRIBUTE_TAG_INFO tag{};
    FILE_STANDARD_INFO before_standard{};
    FILE_BASIC_INFO before_basic{};
    FILE_ID_INFO before_identity{};
    if (!GetFileInformationByHandleEx(
            file.get(), FileAttributeTagInfo, &tag, sizeof(tag)) ||
        !GetFileInformationByHandleEx(
            file.get(), FileStandardInfo, &before_standard,
            sizeof(before_standard)) ||
        !GetFileInformationByHandleEx(
            file.get(), FileBasicInfo, &before_basic, sizeof(before_basic)) ||
        !GetFileInformationByHandleEx(
            file.get(), FileIdInfo, &before_identity,
            sizeof(before_identity))) {
        fail(ClientKeyFailureKind::io);
    }
    if ((tag.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0U ||
        before_standard.Directory != FALSE) {
        fail(ClientKeyFailureKind::unsafe);
    }
    if (before_standard.EndOfFile.QuadPart !=
        static_cast<LONGLONG>(key_file_bytes)) {
        fail(ClientKeyFailureKind::invalid);
    }
    require_restricted_security(file.get());

    std::array<char, key_read_bytes> bytes{};
    std::size_t total = 0;
    while (total < bytes.size()) {
        DWORD observed = 0;
        if (!ReadFile(
                file.get(),
                bytes.data() + total,
                static_cast<DWORD>(bytes.size() - total),
                &observed,
                nullptr)) {
            fail(ClientKeyFailureKind::io);
        }
        if (observed == 0) break;
        total += observed;
    }

    FILE_STANDARD_INFO after_standard{};
    FILE_BASIC_INFO after_basic{};
    FILE_ID_INFO after_identity{};
    if (!GetFileInformationByHandleEx(
            file.get(), FileStandardInfo, &after_standard,
            sizeof(after_standard)) ||
        !GetFileInformationByHandleEx(
            file.get(), FileBasicInfo, &after_basic, sizeof(after_basic)) ||
        !GetFileInformationByHandleEx(
            file.get(), FileIdInfo, &after_identity,
            sizeof(after_identity))) {
        fail(ClientKeyFailureKind::io);
    }
    if (total != key_file_bytes || bytes[key_payload_bytes] != '\n' ||
        before_standard.EndOfFile.QuadPart != after_standard.EndOfFile.QuadPart ||
        before_basic.LastWriteTime.QuadPart != after_basic.LastWriteTime.QuadPart ||
        before_basic.ChangeTime.QuadPart != after_basic.ChangeTime.QuadPart ||
        std::memcmp(&before_identity, &after_identity, sizeof(before_identity)) != 0) {
        fail(ClientKeyFailureKind::invalid);
    }
    std::string value(bytes.data(), key_payload_bytes);
    if (!valid_key(value)) fail(ClientKeyFailureKind::invalid);
    const std::string identity = identity_string(before_identity, before_basic);
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

class WindowsClientKeyStore final : public ports::ClientKeyStore {
  public:
    explicit WindowsClientKeyStore(const std::filesystem::path& state_directory) {
        try {
            root_ = std::filesystem::absolute(state_directory).lexically_normal();
        } catch (const std::filesystem::filesystem_error&) {
            fail(ClientKeyFailureKind::unsafe);
        }
        const std::wstring root_name = root_.root_name().native();
        if (root_.empty() || !root_.is_absolute() ||
            root_ == root_.root_path() || root_name.size() != 2 ||
            root_name[1] != L':' || root_.native().size() > 32'000) {
            fail(ClientKeyFailureKind::unsafe);
        }
        key_path_ = root_ / L"client.key";
    }

    [[nodiscard]] ClientKeyMaterial load_existing() const override {
        const RootAuthority authority = open_root(root_, false);
        return load_key(key_path_, authority);
    }

    [[nodiscard]] ClientKeyMaterial load_or_create(
        const std::string_view proposed_key) override {
        if (!valid_key(proposed_key)) fail(ClientKeyFailureKind::invalid);
        const RootAuthority authority = open_root(root_, true);
        try {
            return load_key(key_path_, authority);
        } catch (const ClientKeyFailure& error) {
            if (error.kind() != ClientKeyFailureKind::missing) throw;
        }

        const std::filesystem::path staged =
            root_ / (L".client.key.stage." +
                     std::filesystem::path(random_suffix()).native());
        PrivateObjectSecurity security(FILE_GENERIC_READ);
        Handle file(CreateFileW(
            staged.c_str(),
            GENERIC_WRITE | FILE_READ_ATTRIBUTES | READ_CONTROL,
            0,
            security.attributes(),
            CREATE_NEW,
            FILE_ATTRIBUTE_TEMPORARY | FILE_ATTRIBUTE_NOT_CONTENT_INDEXED,
            nullptr));
        if (file.get() == INVALID_HANDLE_VALUE) fail(ClientKeyFailureKind::io);

        const std::string serialized = std::string(proposed_key) + '\n';
        std::size_t offset = 0;
        while (offset < serialized.size()) {
            DWORD written = 0;
            if (!WriteFile(
                    file.get(),
                    serialized.data() + offset,
                    static_cast<DWORD>(serialized.size() - offset),
                    &written,
                    nullptr) ||
                written == 0) {
                static_cast<void>(file.close_checked());
                DeleteFileW(staged.c_str());
                fail(ClientKeyFailureKind::io);
            }
            offset += written;
        }
        if (!FlushFileBuffers(file.get()) || !file.close_checked()) {
            DeleteFileW(staged.c_str());
            fail(ClientKeyFailureKind::io);
        }
        if (!MoveFileExW(
                staged.c_str(), key_path_.c_str(), MOVEFILE_WRITE_THROUGH)) {
            DeleteFileW(staged.c_str());
            try {
                return load_key(key_path_, authority);
            } catch (const ClientKeyFailure& error) {
                if (error.kind() != ClientKeyFailureKind::missing) throw;
                fail(ClientKeyFailureKind::io);
            }
        }
        return load_key(key_path_, authority);
    }

  private:
    std::filesystem::path root_;
    std::filesystem::path key_path_;
};

}  // namespace

std::shared_ptr<ports::ClientKeyStore> make_client_key_store(
    const std::filesystem::path& state_directory) {
    return std::make_shared<WindowsClientKeyStore>(state_directory);
}

}  // namespace arena::platform
