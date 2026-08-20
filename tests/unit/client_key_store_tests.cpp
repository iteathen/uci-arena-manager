#include "arena/application/local_auth.hpp"
#include "arena/domain/uuid.hpp"
#include "arena/platform/client_key_store.hpp"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <thread>

#ifdef _WIN32
#define NOMINMAX
#include <aclapi.h>
#include <winioctl.h>
#include <windows.h>
#else
#include <sys/stat.h>
#endif

namespace {

namespace fs = std::filesystem;
using arena::ports::ClientKeyFailure;
using arena::ports::ClientKeyFailureKind;
using arena::ports::ClientKeyMaterial;

constexpr std::string_view first_key =
    "1111111111111111111111111111111111111111111111111111111111111111";
constexpr std::string_view second_key =
    "2222222222222222222222222222222222222222222222222222222222222222";

fs::path unique_root(const std::string& name) {
    return fs::temp_directory_path() /
           (name + "-" + arena::domain::uuid_v7());
}

void rewrite_key(const fs::path& path, const std::string& bytes) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    REQUIRE(output.good());
    output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    output.close();
    REQUIRE(output.good());
}

void require_failure_kind(
    const std::shared_ptr<arena::ports::ClientKeyStore>& store,
    const ClientKeyFailureKind expected) {
    try {
        static_cast<void>(store->load_existing());
        FAIL("unsafe client key was admitted");
    } catch (const ClientKeyFailure& error) {
        REQUIRE(error.kind() == expected);
        REQUIRE(std::string(error.what()) ==
                "client authentication key is unavailable");
        REQUIRE(std::string(error.what()).find(first_key) == std::string::npos);
    }
}

#ifdef _WIN32
void grant_principal_write(
    const fs::path& path,
    const WELL_KNOWN_SID_TYPE principal) {
    std::array<unsigned char, SECURITY_MAX_SID_SIZE> sid{};
    DWORD sid_size = static_cast<DWORD>(sid.size());
    REQUIRE(CreateWellKnownSid(principal, nullptr, sid.data(), &sid_size));
    EXPLICIT_ACCESSW access{};
    access.grfAccessPermissions = FILE_ALL_ACCESS;
    access.grfAccessMode = SET_ACCESS;
    access.grfInheritance = NO_INHERITANCE;
    access.Trustee.TrusteeForm = TRUSTEE_IS_SID;
    access.Trustee.TrusteeType = TRUSTEE_IS_WELL_KNOWN_GROUP;
    access.Trustee.ptstrName = reinterpret_cast<LPWSTR>(sid.data());
    PACL acl = nullptr;
    REQUIRE(SetEntriesInAclW(1, &access, nullptr, &acl) == ERROR_SUCCESS);
    std::wstring native = path.native();
    const DWORD result = SetNamedSecurityInfoW(
        native.data(),
        SE_FILE_OBJECT,
        DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION,
        nullptr,
        nullptr,
        acl,
        nullptr);
    LocalFree(acl);
    REQUIRE(result == ERROR_SUCCESS);
}

void grant_world_write(const fs::path& path) {
    grant_principal_write(path, WinWorldSid);
}

void grant_untrusted_write(const fs::path& path) {
    grant_principal_write(path, WinInteractiveSid);
}

void grant_world_writable_directory(const fs::path& path) {
    grant_world_write(path);
}

bool create_junction(const fs::path& link, const fs::path& target) {
    if (!CreateDirectoryW(link.c_str(), nullptr)) return false;
    const HANDLE directory = CreateFileW(
        link.c_str(),
        GENERIC_WRITE,
        0,
        nullptr,
        OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT,
        nullptr);
    if (directory == INVALID_HANDLE_VALUE) {
        fs::remove(link);
        return false;
    }
    const std::wstring substitute = L"\\??\\" + fs::absolute(target).native();
    const std::wstring display = fs::absolute(target).native();
    constexpr std::size_t path_capacity = 32'768;
    struct MountPointBuffer final {
        DWORD tag;
        WORD data_length;
        WORD reserved;
        WORD substitute_offset;
        WORD substitute_length;
        WORD display_offset;
        WORD display_length;
        WCHAR paths[path_capacity];
    } buffer{};
    const std::size_t substitute_bytes = substitute.size() * sizeof(wchar_t);
    const std::size_t display_bytes = display.size() * sizeof(wchar_t);
    if (substitute_bytes + display_bytes + 2 * sizeof(wchar_t) >
        sizeof(buffer.paths)) {
        CloseHandle(directory);
        fs::remove(link);
        return false;
    }
    buffer.tag = IO_REPARSE_TAG_MOUNT_POINT;
    buffer.substitute_offset = 0;
    buffer.substitute_length = static_cast<WORD>(substitute_bytes);
    buffer.display_offset =
        static_cast<WORD>(substitute_bytes + sizeof(wchar_t));
    buffer.display_length = static_cast<WORD>(display_bytes);
    std::memcpy(buffer.paths, substitute.data(), substitute_bytes);
    std::memcpy(
        reinterpret_cast<unsigned char*>(buffer.paths) + buffer.display_offset,
        display.data(),
        display_bytes);
    constexpr std::size_t header_bytes =
        offsetof(MountPointBuffer, substitute_offset);
    constexpr std::size_t mount_header_bytes =
        offsetof(MountPointBuffer, paths) - header_bytes;
    buffer.data_length = static_cast<WORD>(
        mount_header_bytes + substitute_bytes + sizeof(wchar_t) +
        display_bytes + sizeof(wchar_t));
    DWORD returned = 0;
    const DWORD input_bytes = static_cast<DWORD>(
        offsetof(MountPointBuffer, paths) + substitute_bytes +
        sizeof(wchar_t) + display_bytes + sizeof(wchar_t));
    const bool created = DeviceIoControl(
        directory,
        FSCTL_SET_REPARSE_POINT,
        &buffer,
        input_bytes,
        nullptr,
        0,
        &returned,
        nullptr) != FALSE;
    CloseHandle(directory);
    if (!created) fs::remove(link);
    return created;
}
#else
void grant_world_write(const fs::path& path) {
    REQUIRE(chmod(path.c_str(), S_IRUSR | S_IWUSR | S_IWGRP | S_IWOTH) == 0);
}

void grant_world_writable_directory(const fs::path& path) {
    REQUIRE(chmod(
                path.c_str(),
                S_IRWXU | S_IRWXG | S_IRWXO) == 0);
}
#endif

class ScriptedClientKeyStore final : public arena::ports::ClientKeyStore {
  public:
    [[nodiscard]] ClientKeyMaterial load_existing() const override {
        if (!exists_) throw ClientKeyFailure(ClientKeyFailureKind::missing);
        if (reject_reload_) throw ClientKeyFailure(ClientKeyFailureKind::unsafe);
        return {std::string(second_key), "replacement-generation"};
    }

    [[nodiscard]] ClientKeyMaterial load_or_create(
        std::string_view) override {
        exists_ = true;
        return {std::string(first_key), "initial-generation"};
    }

    bool reject_reload_{};

  private:
    mutable bool exists_{};
};

}  // namespace

TEST_CASE("client key first publication is exact and stable") {
    const fs::path root = unique_root("arena-client-key-create");
    const auto store = arena::platform::make_client_key_store(root);

    const ClientKeyMaterial created = store->load_or_create(first_key);
    const ClientKeyMaterial reopened = store->load_existing();
    REQUIRE(created.value == first_key);
    REQUIRE(reopened.value == first_key);
    REQUIRE(created.object_identity == reopened.object_identity);
    REQUIRE(fs::file_size(root / "client.key") == 65);

    fs::remove_all(root);
}

TEST_CASE("client key root is restricted at first publication") {
    const fs::path parent = unique_root("arena-client-key-broad-parent");
    fs::create_directories(parent);
    grant_world_writable_directory(parent);
    const auto store =
        arena::platform::make_client_key_store(parent / "manager-state");

    REQUIRE(store->load_or_create(first_key).value == first_key);
    REQUIRE(store->load_existing().value == first_key);

    fs::remove_all(parent);
}

TEST_CASE("missing and overlong client key roots fail with bounded diagnostics") {
    const fs::path missing = unique_root("arena-client-key-missing");
    const auto store = arena::platform::make_client_key_store(missing);
    require_failure_kind(store, ClientKeyFailureKind::missing);

#ifdef _WIN32
    const fs::path overlong =
        fs::path("C:\\") / std::string(32'001, 'x');
#else
    const fs::path overlong = fs::path("/") / std::string(4097, 'x');
#endif
    try {
        static_cast<void>(arena::platform::make_client_key_store(overlong));
        FAIL("overlong client key root was accepted");
    } catch (const ClientKeyFailure& error) {
        REQUIRE(error.kind() == ClientKeyFailureKind::unsafe);
        REQUIRE(std::string(error.what()).size() <= 64);
    }
}

TEST_CASE("concurrent first client key creators admit one published generation") {
    const fs::path root = unique_root("arena-client-key-concurrent");
    const auto first = arena::platform::make_client_key_store(root);
    const auto second = arena::platform::make_client_key_store(root);
    std::array<ClientKeyMaterial, 2> results;
    std::array<std::exception_ptr, 2> failures;

    std::thread first_thread([&] {
        try {
            results[0] = first->load_or_create(first_key);
        } catch (...) {
            failures[0] = std::current_exception();
        }
    });
    std::thread second_thread([&] {
        try {
            results[1] = second->load_or_create(second_key);
        } catch (...) {
            failures[1] = std::current_exception();
        }
    });
    first_thread.join();
    second_thread.join();

    REQUIRE_FALSE(failures[0]);
    REQUIRE_FALSE(failures[1]);
    REQUIRE(results[0].value == results[1].value);
    REQUIRE((
        results[0].value == first_key || results[0].value == second_key));
    REQUIRE(first->load_existing().value == results[0].value);

    fs::remove_all(root);
}

TEST_CASE("stale private staging names cannot block client key publication") {
    const fs::path root = unique_root("arena-client-key-stale-stage");
    const auto store = arena::platform::make_client_key_store(root);
    const ClientKeyMaterial created = store->load_or_create(first_key);
    rewrite_key(root / ".client.key.stage.stale", "untrusted-stale-value");

    REQUIRE(store->load_or_create(second_key).value == created.value);
    REQUIRE(fs::is_regular_file(root / ".client.key.stage.stale"));

    fs::remove_all(root);
}

TEST_CASE("client key admission rejects every non-exact byte shape") {
    const std::array invalid_values{
        std::string(first_key),
        std::string(first_key) + "\r\n",
        std::string(first_key) + "\nextra",
        std::string(63, 'a') + "\n",
        std::string(64, 'A') + "\n",
        std::string(4096, 'a'),
    };
    for (const auto& invalid : invalid_values) {
        const fs::path root = unique_root("arena-client-key-shape");
        const auto store = arena::platform::make_client_key_store(root);
        static_cast<void>(store->load_or_create(first_key));
        rewrite_key(root / "client.key", invalid);
        require_failure_kind(store, ClientKeyFailureKind::invalid);
        fs::remove_all(root);
    }
}

TEST_CASE("client key admission rejects directories and broad write authority") {
    {
        const fs::path root = unique_root("arena-client-key-directory");
        const auto store = arena::platform::make_client_key_store(root);
        static_cast<void>(store->load_or_create(first_key));
        fs::remove(root / "client.key");
        fs::create_directory(root / "client.key");
        require_failure_kind(store, ClientKeyFailureKind::unsafe);
        fs::remove_all(root);
    }
    {
        const fs::path root = unique_root("arena-client-key-permissions");
        const auto store = arena::platform::make_client_key_store(root);
        static_cast<void>(store->load_or_create(first_key));
        grant_world_write(root / "client.key");
        require_failure_kind(store, ClientKeyFailureKind::unsafe);
        fs::remove_all(root);
    }
    {
        const fs::path root = unique_root("arena-client-key-root-permissions");
        const auto store = arena::platform::make_client_key_store(root);
        static_cast<void>(store->load_or_create(first_key));
        grant_world_write(root);
        require_failure_kind(store, ClientKeyFailureKind::unsafe);
        fs::remove_all(root);
    }
#ifdef _WIN32
    {
        const fs::path root = unique_root("arena-client-key-untrusted-writer");
        const auto store = arena::platform::make_client_key_store(root);
        static_cast<void>(store->load_or_create(first_key));
        grant_untrusted_write(root / "client.key");
        require_failure_kind(store, ClientKeyFailureKind::unsafe);
        fs::remove_all(root);
    }
#endif
}

TEST_CASE("client key admission rejects final and parent links") {
    const fs::path root = unique_root("arena-client-key-link");
    const fs::path external = unique_root("arena-client-key-external");
    fs::create_directories(root);
    const auto external_store =
        arena::platform::make_client_key_store(external);
    static_cast<void>(external_store->load_or_create(first_key));
#ifdef _WIN32
    if (!create_junction(root / "client.key", external)) {
        fs::remove_all(root);
        fs::remove_all(external);
        SKIP("platform could not create a client-key junction");
    }
#else
    fs::create_symlink(external / "client.key", root / "client.key");
#endif
    const auto linked_key = arena::platform::make_client_key_store(root);
    require_failure_kind(linked_key, ClientKeyFailureKind::unsafe);
    fs::remove(root / "client.key");

    const fs::path linked_root = unique_root("arena-client-key-root-link");
#ifdef _WIN32
    REQUIRE(create_junction(linked_root, external));
#else
    fs::create_directory_symlink(external, linked_root);
#endif
    const auto parent_link =
        arena::platform::make_client_key_store(linked_root);
    require_failure_kind(parent_link, ClientKeyFailureKind::unsafe);
    fs::remove(linked_root);
    fs::remove_all(root);
    fs::remove_all(external);
}

TEST_CASE("LocalAuth admits an exact replacement generation on mismatch") {
    const fs::path root = unique_root("arena-client-key-replacement");
    const fs::path replacement_root =
        unique_root("arena-client-key-replacement-source");
    const auto store = arena::platform::make_client_key_store(root);
    const auto replacement_store =
        arena::platform::make_client_key_store(replacement_root);
    const ClientKeyMaterial first = store->load_or_create(first_key);
    static_cast<void>(replacement_store->load_or_create(second_key));
    arena::application::LocalAuth auth(store);
    auth.initialize();
    REQUIRE(auth.authenticate_bearer("Bearer " + std::string(first_key)));

    fs::remove(root / "client.key");
    fs::rename(
        replacement_root / "client.key", root / "client.key");
    const ClientKeyMaterial admitted_replacement = store->load_existing();
    REQUIRE(admitted_replacement.object_identity != first.object_identity);
    REQUIRE(admitted_replacement.value == second_key);
    REQUIRE(auth.authenticate_bearer("Bearer " + std::string(second_key)));
    REQUIRE_FALSE(auth.authenticate_bearer("Bearer " + std::string(first_key)));

    fs::remove_all(root);
    fs::remove_all(replacement_root);
}

TEST_CASE("LocalAuth retains admitted authority when a reload is rejected") {
    const auto store = std::make_shared<ScriptedClientKeyStore>();
    arena::application::LocalAuth auth(store);
    auth.initialize();
    REQUIRE(auth.authenticate_bearer("Bearer " + std::string(first_key)));

    store->reject_reload_ = true;
    REQUIRE_FALSE(auth.authenticate_bearer("Bearer " + std::string(second_key)));
    REQUIRE(auth.authenticate_bearer("Bearer " + std::string(first_key)));
    REQUIRE_THROWS_AS(
        auth.server_proof(
            std::string(64, 'a'), "http://127.0.0.1:47831"),
        ClientKeyFailure);

    store->reject_reload_ = false;
    REQUIRE(auth.authenticate_bearer("Bearer " + std::string(second_key)));
    REQUIRE_FALSE(auth.authenticate_bearer("Bearer " + std::string(first_key)));
}
