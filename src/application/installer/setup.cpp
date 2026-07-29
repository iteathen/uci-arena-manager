#include "arena/application/installer/setup.hpp"

#include <archive.h>
#include <archive_entry.h>
#include <curl/curl.h>
#include <sodium.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <functional>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <system_error>
#include <thread>
#include <tuple>

#include "arena/adapters/uci_process.hpp"
#include "arena/application/broker_policy.hpp"
#include "arena/application/process_supervisor.hpp"

#ifndef _WIN32
#include <sys/stat.h>
#endif

namespace arena::application::installer {
namespace {

using nlohmann::json;
namespace fs = std::filesystem;

constexpr std::uint64_t gib = 1024ULL * 1024ULL * 1024ULL;
constexpr std::uint64_t managed_book_max_bytes = 256ULL * 1024ULL * 1024ULL;
constexpr std::size_t managed_book_max_files = 64;
constexpr std::uint64_t managed_profile_max_bytes = 64ULL * 1024ULL * 1024ULL;
constexpr std::size_t managed_profile_max_files = 16;
constexpr const char* compiled_tuf_root_sha256 = ARENA_TUF_ROOT_SHA256;

json read_json(const fs::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        throw std::runtime_error("cannot read JSON file: " + path.string());
    }
    try {
        return json::parse(input);
    } catch (const json::exception& error) {
        throw std::runtime_error("invalid JSON in " + path.string() + ": " + error.what());
    }
}

void write_json_atomic(const fs::path& path, const json& value) {
    fs::create_directories(path.parent_path());
    const fs::path temporary = path.string() + ".tmp";
    {
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        if (!output) {
            throw std::runtime_error("cannot write " + temporary.string());
        }
        output << value.dump(2) << '\n';
        output.flush();
        if (!output) {
            throw std::runtime_error("cannot flush " + temporary.string());
        }
    }
    std::error_code error;
    fs::rename(temporary, path, error);
    if (error) {
        fs::remove(path, error);
        error.clear();
        fs::rename(temporary, path, error);
    }
    if (error) {
        throw std::runtime_error("cannot atomically activate " + path.string());
    }
}

std::string sha256_file(const fs::path& path) {
    if (sodium_init() < 0) {
        throw std::runtime_error("libsodium initialization failed");
    }
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        throw std::runtime_error("cannot hash " + path.string());
    }
    crypto_hash_sha256_state state{};
    crypto_hash_sha256_init(&state);
    std::array<unsigned char, 64 * 1024> buffer{};
    while (input) {
        input.read(reinterpret_cast<char*>(buffer.data()), buffer.size());
        const auto count = input.gcount();
        if (count > 0) {
            crypto_hash_sha256_update(&state, buffer.data(),
                                      static_cast<unsigned long long>(count));
        }
    }
    std::array<unsigned char, crypto_hash_sha256_BYTES> digest{};
    crypto_hash_sha256_final(&state, digest.data());
    std::array<char, crypto_hash_sha256_BYTES * 2 + 1> encoded{};
    sodium_bin2hex(encoded.data(), encoded.size(), digest.data(), digest.size());
    return encoded.data();
}

std::string sha256_text(const std::string& value) {
    if (sodium_init() < 0) {
        throw std::runtime_error("libsodium initialization failed");
    }
    std::array<unsigned char, crypto_hash_sha256_BYTES> digest{};
    crypto_hash_sha256(digest.data(), reinterpret_cast<const unsigned char*>(value.data()),
                       static_cast<unsigned long long>(value.size()));
    std::array<char, crypto_hash_sha256_BYTES * 2 + 1> encoded{};
    sodium_bin2hex(encoded.data(), encoded.size(), digest.data(), digest.size());
    return encoded.data();
}

bool valid_identifier(const std::string& value) {
    return !value.empty() &&
           std::all_of(value.begin(), value.end(), [](const unsigned char character) {
               return std::isalnum(character) != 0 || character == '.' || character == '_' ||
                      character == '-';
           });
}

bool valid_bundle_owner(const std::string& value) {
    return value.size() <= 128 &&
           std::all_of(value.begin(), value.end(), [](const unsigned char character) {
               return std::isalnum(character) != 0 || character == '.' || character == '_' ||
                      character == '-' || character == '{' || character == '}';
           });
}

fs::path normalized_absolute(const fs::path& value) {
    return fs::absolute(value).lexically_normal();
}

bool contains_symbolic_link(const fs::path& value) {
    const fs::path absolute = normalized_absolute(value);
    fs::path current = absolute.root_path();
    std::error_code error;
    for (const auto& part : absolute.relative_path()) {
        current /= part;
        const fs::file_status status = fs::symlink_status(current, error);
        if (error) {
            error.clear();
            continue;
        }
        if (fs::is_symlink(status)) {
            return true;
        }
    }
    return false;
}

void require_no_symbolic_link(const fs::path& value) {
    if (contains_symbolic_link(value)) {
        throw std::runtime_error("symbolic-link traversal is forbidden: " + value.string());
    }
}

fs::path nearest_existing_ancestor(fs::path path) {
    path = normalized_absolute(path);
    while (!path.empty() && !fs::exists(path)) {
        const fs::path parent = path.parent_path();
        if (parent == path) {
            break;
        }
        path = parent;
    }
    if (path.empty() || !fs::exists(path)) {
        throw std::runtime_error("cannot resolve a filesystem volume");
    }
    return path;
}

std::string volume_key(const fs::path& path) {
    const fs::path existing = nearest_existing_ancestor(path);
#ifdef _WIN32
    std::string key = (existing.root_name() / existing.root_directory()).string();
    std::ranges::transform(key, key.begin(), [](const unsigned char character) {
        return static_cast<char>(std::tolower(character));
    });
    return key;
#else
    struct stat status{};
    if (::stat(existing.c_str(), &status) != 0) {
        throw std::runtime_error("cannot identify filesystem volume for " + existing.string());
    }
    return std::to_string(static_cast<std::uintmax_t>(status.st_dev));
#endif
}

bool within(const fs::path& root, const fs::path& candidate) {
    const fs::path normalized_root = normalized_absolute(root);
    const fs::path normalized_candidate = normalized_absolute(candidate);
    auto root_it = normalized_root.begin();
    auto candidate_it = normalized_candidate.begin();
    for (; root_it != normalized_root.end(); ++root_it, ++candidate_it) {
        if (candidate_it == normalized_candidate.end() || *root_it != *candidate_it) {
            return false;
        }
    }
    return normalized_candidate != normalized_root;
}

bool safe_relative_path(const fs::path& path) {
    if (path.empty() || path.is_absolute() || path.has_root_name()) {
        return false;
    }
    for (const auto& part : path) {
        if (part == ".." || part == ".") {
            return false;
        }
    }
    return true;
}

std::uint64_t integer_field(const json& value, const char* key) {
    if (!value.contains(key)) {
        return 0;
    }
    if (!value.at(key).is_number_unsigned() && !value.at(key).is_number_integer()) {
        throw std::runtime_error(std::string("catalog field must be an integer: ") + key);
    }
    const auto parsed = value.at(key).get<std::int64_t>();
    if (parsed < 0) {
        throw std::runtime_error(std::string("catalog field must not be negative: ") + key);
    }
    return static_cast<std::uint64_t>(parsed);
}

std::string current_utc_iso() {
    const auto now = std::chrono::system_clock::now();
    const std::time_t timestamp = std::chrono::system_clock::to_time_t(now);
    std::tm utc{};
#ifdef _WIN32
    gmtime_s(&utc, &timestamp);
#else
    gmtime_r(&timestamp, &utc);
#endif
    std::array<char, 32> buffer{};
    std::strftime(buffer.data(), buffer.size(), "%Y-%m-%dT%H:%M:%SZ", &utc);
    return buffer.data();
}

std::vector<unsigned char> decode_hex(const std::string& encoded, const std::size_t expected_size,
                                      const std::string& label) {
    std::vector<unsigned char> decoded(expected_size);
    std::size_t actual = 0;
    if (sodium_hex2bin(decoded.data(), decoded.size(), encoded.c_str(), encoded.size(), nullptr,
                       &actual, nullptr) != 0 ||
        actual != expected_size) {
        throw std::runtime_error("invalid hexadecimal " + label);
    }
    return decoded;
}

void verify_role(const json& metadata, const json& root_signed, const std::string& role_name) {
    if (!metadata.contains("signed") || !metadata.contains("signatures") ||
        !metadata.at("signatures").is_array()) {
        throw std::runtime_error("TUF " + role_name + " metadata is incomplete");
    }
    const auto& role = root_signed.at("roles").at(role_name);
    const std::set<std::string> allowed(role.at("keyids").begin(), role.at("keyids").end());
    const std::size_t threshold = role.at("threshold").get<std::size_t>();
    const std::string canonical = metadata.at("signed").dump();
    std::set<std::string> accepted;
    for (const auto& signature : metadata.at("signatures")) {
        const std::string key_id = signature.value("keyid", "");
        if (!allowed.contains(key_id) || accepted.contains(key_id)) {
            continue;
        }
        const auto& key = root_signed.at("keys").at(key_id);
        if (key.value("keytype", "") != "ed25519" || key.value("scheme", "") != "ed25519") {
            continue;
        }
        const auto public_key = decode_hex(key.at("keyval").at("public").get<std::string>(),
                                           crypto_sign_PUBLICKEYBYTES, "TUF public key");
        const auto signature_bytes =
            decode_hex(signature.at("sig").get<std::string>(), crypto_sign_BYTES, "TUF signature");
        if (crypto_sign_verify_detached(signature_bytes.data(),
                                        reinterpret_cast<const unsigned char*>(canonical.data()),
                                        canonical.size(), public_key.data()) == 0) {
            accepted.insert(key_id);
        }
    }
    if (accepted.size() < threshold) {
        throw std::runtime_error("TUF " + role_name + " signature threshold was not met");
    }
}

void verify_not_expired(const json& metadata, const std::string& role) {
    const std::string expires = metadata.at("signed").value("expires", "");
    if (expires.size() != 20 || expires <= current_utc_iso()) {
        throw std::runtime_error("TUF " + role + " metadata is expired or malformed");
    }
}

void verify_described_file(const fs::path& path, const json& descriptor, const std::string& label) {
    if (!fs::is_regular_file(path)) {
        throw std::runtime_error(label + " is missing: " + path.string());
    }
    const auto expected_size = integer_field(descriptor, "length");
    if (expected_size != 0 && fs::file_size(path) != expected_size) {
        throw std::runtime_error(label + " size does not match trusted metadata");
    }
    const std::string expected_hash =
        descriptor.value("hashes", json::object()).value("sha256", "");
    if (expected_hash.size() != 64 || sha256_file(path) != expected_hash) {
        throw std::runtime_error(label + " SHA-256 does not match trusted metadata");
    }
}

std::uint64_t metadata_version(const json& metadata, const std::string& role) {
    if (!metadata.at("signed").contains("version") ||
        (!metadata.at("signed").at("version").is_number_unsigned() &&
         !metadata.at("signed").at("version").is_number_integer())) {
        throw std::runtime_error("TUF " + role + " version is missing");
    }
    const auto version = metadata.at("signed").at("version").get<std::int64_t>();
    if (version < 1) {
        throw std::runtime_error("TUF " + role + " version is invalid");
    }
    return static_cast<std::uint64_t>(version);
}

void verify_tuf_chain(const fs::path& catalog_path, const fs::path& metadata_directory,
                      const fs::path& rollback_state_path) {
    if (sodium_init() < 0) {
        throw std::runtime_error("libsodium initialization failed");
    }
    const fs::path root_path = metadata_directory / "root.json";
    const std::string pinned_root = compiled_tuf_root_sha256;
    if (pinned_root.size() != 64 || sha256_file(root_path) != pinned_root) {
        throw std::runtime_error(
            "TUF root metadata does not match the root compiled into arena-setup");
    }
    const json root = read_json(root_path);
    if (root.at("signed").value("_type", "") != "root") {
        throw std::runtime_error("trusted root metadata has the wrong type");
    }
    verify_role(root, root.at("signed"), "root");
    verify_not_expired(root, "root");

    const json timestamp = read_json(metadata_directory / "timestamp.json");
    verify_role(timestamp, root.at("signed"), "timestamp");
    verify_not_expired(timestamp, "timestamp");
    verify_described_file(metadata_directory / "snapshot.json",
                          timestamp.at("signed").at("meta").at("snapshot.json"), "TUF snapshot");

    const json snapshot = read_json(metadata_directory / "snapshot.json");
    verify_role(snapshot, root.at("signed"), "snapshot");
    verify_not_expired(snapshot, "snapshot");
    verify_described_file(metadata_directory / "targets.json",
                          snapshot.at("signed").at("meta").at("targets.json"), "TUF targets");

    const json targets = read_json(metadata_directory / "targets.json");
    verify_role(targets, root.at("signed"), "targets");
    verify_not_expired(targets, "targets");
    const std::string target_name = catalog_path.filename().generic_string();
    verify_described_file(catalog_path, targets.at("signed").at("targets").at(target_name),
                          "installation catalog");

    const json versions{
        {"root", metadata_version(root, "root")},
        {"timestamp", metadata_version(timestamp, "timestamp")},
        {"snapshot", metadata_version(snapshot, "snapshot")},
        {"targets", metadata_version(targets, "targets")},
    };
    if (timestamp.at("signed").at("meta").at("snapshot.json").value("version", 0ULL) !=
            versions.at("snapshot").get<std::uint64_t>() ||
        snapshot.at("signed").at("meta").at("targets.json").value("version", 0ULL) !=
            versions.at("targets").get<std::uint64_t>()) {
        throw std::runtime_error("TUF delegated metadata versions disagree");
    }
    if (!rollback_state_path.empty() && fs::is_regular_file(rollback_state_path)) {
        const json previous = read_json(rollback_state_path);
        if (previous.value("schema", "") != "arena_tuf_rollback_state_v1") {
            throw std::runtime_error("TUF rollback state is malformed");
        }
        for (const auto& [role, version] : versions.items()) {
            if (version.get<std::uint64_t>() < previous.value(role, 0ULL)) {
                throw std::runtime_error("TUF rollback detected for " + role);
            }
        }
    }
    if (!rollback_state_path.empty()) {
        write_json_atomic(rollback_state_path, {
                                                   {"schema", "arena_tuf_rollback_state_v1"},
                                                   {"root_sha256", pinned_root},
                                                   {"root", versions.at("root")},
                                                   {"timestamp", versions.at("timestamp")},
                                                   {"snapshot", versions.at("snapshot")},
                                                   {"targets", versions.at("targets")},
                                                   {"verified_at", current_utc_iso()},
                                               });
    }
}

const json& component_by_id(const json& catalog, const std::string& id) {
    for (const auto& component : catalog.at("components")) {
        if (component.value("id", "") == id) {
            return component;
        }
    }
    throw std::runtime_error("catalog does not define component " + id);
}

const json& platform_artifact(const json& component) {
    const auto& artifacts = component.at("artifacts");
    const std::string key = SetupEngine::platform_key();
    if (artifacts.contains(key)) {
        return artifacts.at(key);
    }
    if (artifacts.contains("any")) {
        return artifacts.at("any");
    }
    throw std::runtime_error("component " + component.at("id").get<std::string>() +
                             " has no artifact for " + key);
}

std::size_t curl_write(char* data, std::size_t size, std::size_t count, void* output) {
    static_cast<std::ofstream*>(output)->write(data, static_cast<std::streamsize>(size * count));
    return size * count;
}

fs::path acquire_artifact(const json& artifact, const fs::path& catalog_directory,
                          const SetupPaths& paths, const bool offline) {
    fs::create_directories(paths.cache_root);
    const std::string expected_hash = artifact.at("sha256").get<std::string>();
    if (expected_hash.size() != 64) {
        throw std::runtime_error("artifact SHA-256 is malformed");
    }
    const fs::path cached = paths.cache_root / expected_hash;
    if (fs::is_regular_file(cached)) {
        if (sha256_file(cached) == expected_hash) {
            return cached;
        }
        std::error_code remove_error;
        fs::remove(cached, remove_error);
        if (remove_error) {
            throw std::runtime_error("cannot replace corrupt cached artifact");
        }
    }

    const std::string source = artifact.at("source").get<std::string>();
    const fs::path temporary = cached.string() + ".partial";
    std::error_code ignored;
    if (source.starts_with("https://")) {
        if (offline) {
            throw std::runtime_error("offline mode cannot download " + source);
        }
        const std::uintmax_t partial_size =
            fs::is_regular_file(temporary) ? fs::file_size(temporary) : 0;
        const auto expected_size = integer_field(artifact, "size_bytes");
        if (expected_size != 0 && partial_size > expected_size) {
            fs::remove(temporary, ignored);
        }
        const std::uintmax_t resume_from =
            fs::is_regular_file(temporary) ? fs::file_size(temporary) : 0;
        if (expected_size != 0 && resume_from == expected_size &&
            sha256_file(temporary) == expected_hash) {
            fs::rename(temporary, cached);
            return cached;
        }
        std::ofstream output(temporary, std::ios::binary |
                                            (resume_from > 0 ? std::ios::app : std::ios::trunc));
        CURL* handle = curl_easy_init();
        if (handle == nullptr) {
            throw std::runtime_error("cannot initialize artifact download");
        }
        curl_easy_setopt(handle, CURLOPT_URL, source.c_str());
        curl_easy_setopt(handle, CURLOPT_FOLLOWLOCATION, 1L);
        curl_easy_setopt(handle, CURLOPT_PROTOCOLS_STR, "https");
        curl_easy_setopt(handle, CURLOPT_CONNECTTIMEOUT_MS, 10000L);
        curl_easy_setopt(handle, CURLOPT_TIMEOUT, 0L);
        curl_easy_setopt(handle, CURLOPT_FAILONERROR, 1L);
        if (resume_from > 0) {
            curl_easy_setopt(handle, CURLOPT_RESUME_FROM_LARGE,
                             static_cast<curl_off_t>(resume_from));
        }
        curl_easy_setopt(handle, CURLOPT_WRITEFUNCTION, curl_write);
        curl_easy_setopt(handle, CURLOPT_WRITEDATA, &output);
        const CURLcode result = curl_easy_perform(handle);
        curl_easy_cleanup(handle);
        output.close();
        if (result != CURLE_OK) {
            throw std::runtime_error("artifact download failed: " +
                                     std::string(curl_easy_strerror(result)));
        }
    } else {
        fs::remove(temporary, ignored);
        fs::path source_path = fs::path(source);
        if (source_path.is_relative()) {
            source_path = catalog_directory / source_path;
        }
        if (!fs::is_regular_file(source_path)) {
            throw std::runtime_error("artifact source is unavailable: " + source_path.string());
        }
        fs::copy_file(source_path, temporary, fs::copy_options::overwrite_existing);
    }
    if (sha256_file(temporary) != expected_hash) {
        fs::remove(temporary, ignored);
        throw std::runtime_error("downloaded artifact SHA-256 mismatch");
    }
    const auto expected_size = integer_field(artifact, "size_bytes");
    if (expected_size != 0 && fs::file_size(temporary) != expected_size) {
        fs::remove(temporary, ignored);
        throw std::runtime_error("downloaded artifact size mismatch");
    }
    fs::rename(temporary, cached);
    return cached;
}

json stage_file_set(const fs::path& manifest_path, const json& catalog_artifact,
                    const fs::path& catalog_directory, const SetupPaths& paths, const bool offline,
                    const fs::path& stage, const std::string& component_id,
                    const std::string& component_version) {
    const json file_set = read_json(manifest_path);
    const bool provider_syzygy =
        file_set.value("schema", "") == "arena_syzygy_file_set_manifest_v1";
    const bool generic_file_set = file_set.value("schema", "") == "arena_install_file_set_v1";
    if ((!provider_syzygy && !generic_file_set) || file_set.value("schema_version", 0) != 1 ||
        file_set.value("component_id", "") != component_id ||
        file_set.value("component_version", "") != component_version ||
        !file_set.contains("files") || !file_set.at("files").is_array()) {
        throw std::runtime_error("file-set manifest identity is invalid");
    }
    const auto expected_count = integer_field(catalog_artifact, "file_count");
    const auto expected_total = integer_field(catalog_artifact, "total_size_bytes");
    if (expected_count == 0 || file_set.at("files").size() != expected_count) {
        throw std::runtime_error("file-set manifest count differs from signed catalog");
    }

    std::uint64_t total = 0;
    std::set<fs::path> paths_seen;
    json installed_files = json::array();
    fs::create_directories(stage);
    for (const auto& file : file_set.at("files")) {
        const fs::path declared = fs::path(file.at("path").get<std::string>()).lexically_normal();
        const fs::path relative = provider_syzygy ? fs::path("tablebases") / declared : declared;
        if (!safe_relative_path(relative) || !paths_seen.insert(relative).second ||
            !within(stage, stage / relative)) {
            throw std::runtime_error("file-set manifest contains an unsafe or duplicate path");
        }
        if (component_id.rfind("syzygy.", 0) == 0) {
            const auto generic = relative.generic_string();
            const auto extension = relative.extension().string();
            if (!generic.starts_with("tablebases/") ||
                (extension != ".rtbw" && extension != ".rtbz")) {
                throw std::runtime_error(
                    "Syzygy file-set entries must be tablebases/*.rtbw or *.rtbz");
            }
        }
        const auto size = integer_field(file, "size_bytes");
        if (size == 0 || total > (std::numeric_limits<std::uint64_t>::max)() - size) {
            throw std::runtime_error("file-set manifest has invalid total size");
        }
        total += size;
        fs::path cached;
        if (provider_syzygy) {
            if (!file.contains("sources") || !file.at("sources").is_array() ||
                file.at("sources").empty()) {
                throw std::runtime_error("Syzygy file-set entry has no acquisition source");
            }
            std::string last_error;
            for (const auto& source : file.at("sources")) {
                try {
                    cached = acquire_artifact(
                        {{"source", source}, {"sha256", file.at("sha256")}, {"size_bytes", size}},
                        catalog_directory, paths, offline);
                    break;
                } catch (const std::exception& error) {
                    last_error = error.what();
                }
            }
            if (cached.empty()) {
                throw std::runtime_error("all Syzygy acquisition sources failed for " +
                                         declared.generic_string() + ": " + last_error);
            }
        } else {
            cached = acquire_artifact(file, catalog_directory, paths, offline);
        }
        fs::create_directories((stage / relative).parent_path());
        fs::copy_file(cached, stage / relative, fs::copy_options::overwrite_existing);
        installed_files.push_back({{"path", relative.generic_string()},
                                   {"sha256", file.at("sha256")},
                                   {"size_bytes", size}});
    }
    if (total != expected_total ||
        file_set.value("total_size_bytes", std::uint64_t{0}) != expected_total) {
        throw std::runtime_error("file-set manifest bytes differ from signed catalog");
    }

    const json component_manifest{
        {"schema", "arena_install_component_v1"},
        {"schema_version", 1},
        {"component_id", component_id},
        {"component_version", component_version},
        {"target_triple", "any"},
        {"entrypoints", json::object()},
        {"capabilities", json::array({"syzygy"})},
        {"startup", json::object()},
        {"data_paths", json::array()},
        {"files", installed_files},
    };
    write_json_atomic(stage / "arena-component.json", component_manifest);
    return component_manifest;
}

void extract_archive(const fs::path& archive_path, const fs::path& destination) {
    fs::create_directories(destination);
    archive* reader = archive_read_new();
    archive_read_support_filter_all(reader);
    archive_read_support_format_all(reader);
    if (archive_read_open_filename(reader, archive_path.string().c_str(), 10240) != ARCHIVE_OK) {
        const std::string error = archive_error_string(reader);
        archive_read_free(reader);
        throw std::runtime_error("cannot open component archive: " + error);
    }
    archive_entry* entry = nullptr;
    try {
        while (archive_read_next_header(reader, &entry) == ARCHIVE_OK) {
            const char* raw_path = archive_entry_pathname(entry);
            const fs::path relative =
                raw_path == nullptr ? fs::path{} : fs::path(raw_path).lexically_normal();
            if (relative == fs::path(".") && archive_entry_filetype(entry) == AE_IFDIR) {
                archive_read_data_skip(reader);
                continue;
            }
            if (!safe_relative_path(relative)) {
                throw std::runtime_error("component archive contains an unsafe path");
            }
            const fs::path output_path = destination / relative;
            if (!within(destination, output_path)) {
                throw std::runtime_error("component archive escapes its staging root");
            }
            archive_entry_set_pathname(entry, output_path.string().c_str());
            const int result = archive_read_extract(reader, entry,
                                                    ARCHIVE_EXTRACT_TIME | ARCHIVE_EXTRACT_PERM |
                                                        ARCHIVE_EXTRACT_SECURE_NODOTDOT |
                                                        ARCHIVE_EXTRACT_SECURE_SYMLINKS);
            if (result != ARCHIVE_OK) {
                throw std::runtime_error("component extraction failed: " +
                                         std::string(archive_error_string(reader)));
            }
        }
        archive_read_close(reader);
        archive_read_free(reader);
    } catch (...) {
        archive_read_close(reader);
        archive_read_free(reader);
        throw;
    }
}

json validate_component_payload(const fs::path& root, const std::string& expected_id,
                                const std::string& expected_version) {
    const json manifest = read_json(root / "arena-component.json");
    if (manifest.value("schema", "") != "arena_install_component_v1" ||
        manifest.value("component_id", "") != expected_id ||
        manifest.value("component_version", "") != expected_version) {
        throw std::runtime_error("component payload identity does not match the catalog");
    }
    std::set<fs::path> listed_files;
    for (const auto& file : manifest.at("files")) {
        const fs::path relative = file.at("path").get<std::string>();
        if (!safe_relative_path(relative)) {
            throw std::runtime_error("component manifest contains an unsafe path");
        }
        if (!listed_files.insert(relative.lexically_normal()).second) {
            throw std::runtime_error("component manifest contains a duplicate path");
        }
        const fs::path path = root / relative;
        if (!fs::is_regular_file(path) ||
            fs::file_size(path) != integer_field(file, "size_bytes") ||
            sha256_file(path) != file.at("sha256").get<std::string>()) {
            throw std::runtime_error("component payload verification failed for " +
                                     relative.generic_string());
        }
    }
    for (const auto& entry : fs::recursive_directory_iterator(root)) {
        if (entry.is_symlink()) {
            throw std::runtime_error("component payload contains a symbolic link");
        }
        if (!entry.is_regular_file()) {
            continue;
        }
        const fs::path relative = fs::relative(entry.path(), root).lexically_normal();
        if (relative != fs::path("arena-component.json") && !listed_files.contains(relative)) {
            throw std::runtime_error("component payload contains an unlisted file: " +
                                     relative.generic_string());
        }
    }
    return manifest;
}

void copy_directory_contents(const fs::path& source, const fs::path& destination,
                             std::vector<fs::path>& copied_files) {
    if (!fs::is_directory(source)) {
        throw std::runtime_error("knowledge component is missing " + source.filename().string());
    }
    fs::create_directories(destination);
    for (const auto& entry : fs::recursive_directory_iterator(source)) {
        if (entry.is_symlink()) {
            throw std::runtime_error("knowledge payload contains a symbolic link");
        }
        const fs::path relative = fs::relative(entry.path(), source);
        const fs::path target = destination / relative;
        if (!within(destination, target)) {
            throw std::runtime_error("knowledge payload escapes its activation root");
        }
        if (entry.is_directory()) {
            fs::create_directories(target);
        } else if (entry.is_regular_file()) {
            fs::create_directories(target.parent_path());
            fs::copy_file(entry.path(), target, fs::copy_options::overwrite_existing);
            copied_files.push_back(relative);
        } else {
            throw std::runtime_error("knowledge payload contains an unsupported entry");
        }
    }
}

json activate_knowledge(const fs::path& component_root, const json& manifest,
                        const SetupPaths& paths, const std::string& transaction_id,
                        const std::string& component_id) {
    std::optional<std::pair<fs::path, fs::path>> mapping;
    if (component_id.rfind("opening_book.", 0) == 0) {
        mapping =
            std::pair{component_root / "opening_books", paths.knowledge_root / "opening_books"};
    } else if (component_id.rfind("syzygy.", 0) == 0) {
        mapping = std::pair{component_root / "tablebases", paths.knowledge_root / "tablebases"};
    }
    if (!mapping.has_value()) {
        return json::object();
    }

    const auto& [source, destination] = *mapping;
    fs::create_directories(destination.parent_path());
    const fs::path candidate =
        destination.parent_path() /
        (destination.filename().string() + ".staging-" + transaction_id + "-" + component_id);
    const fs::path backup =
        destination.parent_path() /
        (destination.filename().string() + ".rollback-" + transaction_id + "-" + component_id);
    std::error_code ignored;
    fs::remove_all(candidate, ignored);
    fs::remove_all(backup, ignored);
    std::vector<fs::path> copied_files;
    if (fs::exists(destination)) {
        copy_directory_contents(destination, candidate, copied_files);
        copied_files.clear();
    }
    copy_directory_contents(source, candidate, copied_files);
    const bool had_existing = fs::exists(destination);
    if (had_existing) {
        fs::rename(destination, backup);
    }
    try {
        fs::rename(candidate, destination);
    } catch (...) {
        if (had_existing && fs::exists(backup) && !fs::exists(destination)) {
            fs::rename(backup, destination);
        }
        throw;
    }

    json activated_files = json::array();
    const std::string manifest_prefix = source.filename().generic_string() + "/";
    for (const auto& file : manifest.at("files")) {
        const std::string relative = file.at("path").get<std::string>();
        if (relative.rfind(manifest_prefix, 0) != 0) {
            continue;
        }
        activated_files.push_back(
            {{"path", (destination / fs::path(relative.substr(manifest_prefix.size()))).string()},
             {"sha256", file.at("sha256")},
             {"size_bytes", file.at("size_bytes")}});
    }
    return {
        {"path", destination.string()},
        {"backup_path", had_existing ? backup.string() : ""},
        {"had_existing", had_existing},
        {"activated_files", std::move(activated_files)},
    };
}

std::vector<std::string> selected_ids(const json& catalog, const std::string& preset,
                                      const std::vector<std::string>& selected,
                                      const bool download_missing_knowledge,
                                      const bool download_missing_books,
                                      const bool download_missing_tablebases) {
    std::vector<std::string> result;
    if (preset == "custom") {
        result = selected;
    } else {
        if (!catalog.at("presets").contains(preset)) {
            throw std::runtime_error("unknown installation preset " + preset);
        }
        result = catalog.at("presets").at(preset).get<std::vector<std::string>>();
    }
    const auto select_once = [&result](const std::string& id) {
        if (std::find(result.begin(), result.end(), id) == result.end()) {
            result.push_back(id);
        }
    };
    if (download_missing_knowledge || download_missing_books) {
        select_once("opening_book.standard");
    }
    if (download_missing_knowledge || download_missing_tablebases) {
        select_once("syzygy.wdl.3-4-5");
    }
    if (std::find(result.begin(), result.end(), "manager.core") == result.end()) {
        result.push_back("manager.core");
    }

    std::map<std::string, int> visits;
    std::vector<std::string> ordered;
    std::function<void(const std::string&)> add = [&](const std::string& id) {
        if (!valid_identifier(id)) {
            throw std::runtime_error("invalid component id " + id);
        }
        if (visits[id] == 1) {
            throw std::runtime_error("component dependency cycle at " + id);
        }
        if (visits[id] == 2) {
            return;
        }
        visits[id] = 1;
        const auto& component = component_by_id(catalog, id);
        for (const auto& dependency : component.value("dependencies", json::array())) {
            add(dependency.get<std::string>());
        }
        visits[id] = 2;
        ordered.push_back(id);
    };
    for (const auto& id : result) {
        add(id);
    }
    return ordered;
}

json receipt_rows(const SetupPaths& paths) {
    json rows = json::array();
    const fs::path directory = paths.data_root / "receipts";
    if (!fs::is_directory(directory) || contains_symbolic_link(directory)) {
        return rows;
    }
    for (const auto& entry : fs::directory_iterator(directory)) {
        if (entry.is_regular_file() && !entry.is_symlink() && entry.path().extension() == ".json") {
            try {
                rows.push_back(read_json(entry.path()));
            } catch (const std::exception& error) {
                rows.push_back({{"schema", "arena_install_receipt_v1"},
                                {"state", "broken"},
                                {"receipt", entry.path().string()},
                                {"error", error.what()}});
            }
        }
    }
    return rows;
}

const std::array<std::string_view, 11> discovery_kinds{
    "opening_book",
    "syzygy",
    "uci_engine",
    "book_forge",
    "evidence_service",
    "timing_profile",
    "lichess_single_game_profile",
    "lichess_capacity_profile",
    "lichess_transport_profile",
    "lichess_token_path",
    "restaurant",
};

bool valid_discovery_kind(const std::string_view kind) {
    return std::find(discovery_kinds.begin(), discovery_kinds.end(), kind) != discovery_kinds.end();
}

json browse_contract(const std::string_view kind) {
    json result{
        {"selection", "file"},
        {"filters", json::array()},
        {"hints", json::array()},
        {"managed_copy_allowed", false},
    };
    if (kind == "opening_book") {
        result["selection"] = "file_or_directory";
        result["filters"].push_back(
            {{"label", "Opening books"}, {"patterns", {"*.bin", "*.book"}}});
        result["hints"] = {
            "Select a book file or a flat directory containing its sidecars.",
            "Reference in place is the default; managed copy is limited to 256 "
            "MiB.",
        };
        result["managed_copy_allowed"] = true;
    } else if (kind == "syzygy") {
        result["selection"] = "directory";
        result["filters"].push_back(
            {{"label", "Syzygy tablebases"}, {"patterns", {"*.rtbw", "*.rtbz"}}});
        result["hints"] = {
            "Select the directory containing the tablebase files.",
            "Tablebases are always referenced in place.",
        };
    } else if (kind == "uci_engine") {
#ifdef _WIN32
        result["filters"].push_back({{"label", "UCI engine executables"}, {"patterns", {"*.exe"}}});
#else
        result["filters"].push_back({{"label", "UCI engine executables"}, {"patterns", {"*"}}});
#endif
        result["hints"] = {
            "Any executable is accepted only after a complete bounded UCI probe.",
        };
    } else if (kind == "book_forge") {
        result["selection"] = "file_or_directory";
        result["filters"].push_back(
            {{"label", "Book Forge provider"}, {"patterns", {"arena-component.json", "*.mjs"}}});
        result["hints"] = {
            "Select a provider root containing arena-component.json or its CLI "
            "entrypoint.",
        };
    } else if (kind == "evidence_service") {
        result["selection"] = "file_or_directory";
        result["filters"].push_back({{"label", "Evidence Service provider"},
                                     {"patterns", {"arena-component.json", "*.mjs"}}});
        result["hints"] = {
            "Select a provider root containing arena-component.json or its CLI "
            "entrypoint.",
        };
    } else if (kind == "timing_profile") {
        result["filters"].push_back({{"label", "Qualified UCI timing profile"},
                                     {"patterns", {"uci_time_profile_v3.json", "*.json"}}});
        result["hints"] = {
            "Select the exact qualified uci_time_profile_v3.json published for "
            "this engine, model, "
            "provider, and hardware.",
            "The profile remains an in-place reference and is rejected at runtime "
            "if its identity "
            "is stale.",
        };
    } else if (kind == "lichess_single_game_profile") {
        result["filters"].push_back(
            {{"label", "Qualified Lichess single-game profile"},
             {"patterns", {"uci_arena_single_game_profile_v1.json", "*.json"}}});
        result["hints"] = {
            "Select the qualified single-game profile for the exact installed engine, model, "
            "provider, and hardware.",
            "The profile and its bounded hash-verified evidence closure are copied unchanged "
            "into immutable Manager storage.",
        };
        result["managed_copy_allowed"] = true;
    } else if (kind == "lichess_capacity_profile") {
        result["filters"].push_back(
            {{"label", "Qualified Lichess capacity profile"},
             {"patterns", {"uci_arena_lichess_capacity_profile_v1.json", "*.json"}}});
        result["hints"] = {
            "Select the qualified capacity profile that matches the single-game profile and "
            "configured concurrency.",
            "The profile remains in place and is never rewritten.",
        };
    } else if (kind == "lichess_transport_profile") {
        result["filters"].push_back(
            {{"label", "Qualified Lichess transport profile"},
             {"patterns", {"transport_profile_v2.json", "*.json"}}});
        result["hints"] = {
            "Select the qualified Lichess transport-reserve profile for this host.",
            "The profile remains in place and is never rewritten.",
        };
    } else if (kind == "lichess_token_path") {
        result["filters"].push_back(
            {{"label", "Token reference files"}, {"patterns", {"*.txt", "*.token", "*"}}});
        result["hints"] = {
            "Only the path is saved; token contents are never opened, copied, "
            "hashed, logged, or "
            "returned.",
        };
    } else if (kind == "restaurant") {
        result["selection"] = "directory";
        result["hints"] = {
            "Restaurant is detect-only and cannot be imported, installed, or "
            "controlled.",
        };
    }
    return result;
}

std::optional<std::string> discovery_kind_for_component(const std::string& component_id) {
    if (component_id.rfind("opening_book.", 0) == 0) {
        return "opening_book";
    }
    if (component_id.rfind("syzygy.", 0) == 0) {
        return "syzygy";
    }
    if (component_id == "book_forge.service") {
        return "book_forge";
    }
    if (component_id == "uci_arena_evidence.service") {
        return "evidence_service";
    }
    if (component_id == "restaurant.training_system") {
        return "restaurant";
    }
    if (component_id == "uci_arena.engine" || component_id == "stockfish.engine" ||
        component_id == "lc0.engine" || component_id.ends_with(".uci_engine")) {
        return "uci_engine";
    }
    return std::nullopt;
}

fs::path receipt_locator(const json& receipt) {
    if (receipt.contains("locator_path") && receipt.at("locator_path").is_string()) {
        return receipt.at("locator_path").get<std::string>();
    }
    if (receipt.contains("active_root") && receipt.at("active_root").is_string()) {
        return receipt.at("active_root").get<std::string>();
    }
    if (receipt.contains("activated_files") && receipt.at("activated_files").is_array() &&
        !receipt.at("activated_files").empty() &&
        receipt.at("activated_files").at(0).contains("path")) {
        return fs::path(receipt.at("activated_files").at(0).at("path").get<std::string>())
            .parent_path();
    }
    return {};
}

void append_discovery_candidate(json& rows, std::map<std::string, std::set<std::string>>& seen,
                                const std::string& kind, const std::string& source,
                                const fs::path& path, const std::string& state,
                                const json& details = json::object()) {
    if (!valid_discovery_kind(kind) || path.empty()) {
        return;
    }
    const std::string normalized = normalized_absolute(path).string();
    const std::string key = source + "\n" + normalized;
    if (!seen[kind].insert(key).second) {
        return;
    }
    json candidate{
        {"source", source},
        {"state", state},
        {"path", normalized},
    };
    if (!details.empty()) {
        candidate["details"] = details;
    }
    rows.at(kind).at("candidates").push_back(candidate);
    if (rows.at(kind).at("selection").is_null()) {
        rows.at(kind)["selection"] = std::move(candidate);
    }
}

void inspect_provider_root(json& rows, std::map<std::string, std::set<std::string>>& seen,
                           const fs::path& root) {
    if (!fs::is_directory(root) || contains_symbolic_link(root)) {
        return;
    }
    const fs::path manifest_path = root / "arena-component.json";
    std::error_code error;
    if (!fs::is_regular_file(manifest_path) ||
        fs::is_symlink(fs::symlink_status(manifest_path, error)) || error ||
        fs::file_size(manifest_path, error) > 1024ULL * 1024ULL || error) {
        return;
    }
    try {
        const json manifest = read_json(manifest_path);
        if (manifest.value("schema", "") != "arena_install_component_v1") {
            return;
        }
        const std::string component_id = manifest.value("component_id", "");
        const auto kind = discovery_kind_for_component(component_id);
        if (!kind) {
            return;
        }
        append_discovery_candidate(
            rows, seen, *kind, "provider_manifest", root, "detected_unmanaged",
            {{"component_id", component_id},
             {"component_version", manifest.value("component_version", "")}});
    } catch (const std::exception&) {
    }
}

std::vector<fs::path> bounded_root_candidates(const std::vector<fs::path>& roots) {
    std::map<std::string, fs::path> candidates;
    for (const auto& candidate_root : roots) {
        if (candidate_root.empty()) {
            continue;
        }
        const fs::path root = normalized_absolute(candidate_root);
        if (contains_symbolic_link(root)) {
            continue;
        }
        if (fs::is_regular_file(root)) {
            candidates.emplace(root.string(), root);
            continue;
        }
        if (!fs::is_directory(root)) {
            continue;
        }
        candidates.emplace(root.string(), root);
        std::error_code error;
        for (fs::directory_iterator
                 iterator(root, fs::directory_options::skip_permission_denied, error),
             end;
             iterator != end && !error; iterator.increment(error)) {
            if (iterator->is_directory(error) && !error && !iterator->is_symlink(error) && !error &&
                !contains_symbolic_link(iterator->path())) {
                const fs::path child = normalized_absolute(iterator->path());
                candidates.emplace(child.string(), child);
            }
            error.clear();
        }
    }
    std::vector<fs::path> result;
    result.reserve(candidates.size());
    for (auto& [key, path] : candidates) {
        (void)key;
        result.push_back(std::move(path));
    }
    return result;
}

void inspect_provider_roots(json& rows, std::map<std::string, std::set<std::string>>& seen,
                            const std::vector<fs::path>& roots) {
    for (const auto& root : bounded_root_candidates(roots)) {
        if (!fs::is_directory(root)) {
            continue;
        }
        inspect_provider_root(rows, seen, root);
    }
}

bool directory_has_extension(const fs::path& root, const std::set<std::string>& extensions) {
    if (!fs::is_directory(root) || contains_symbolic_link(root)) {
        return false;
    }
    std::error_code error;
    for (fs::directory_iterator
             iterator(root, fs::directory_options::skip_permission_denied, error),
         end;
         iterator != end && !error; iterator.increment(error)) {
        if (!iterator->is_regular_file(error) || error || iterator->is_symlink(error) || error) {
            error.clear();
            continue;
        }
        std::string extension = iterator->path().extension().string();
        std::ranges::transform(extension, extension.begin(), [](const unsigned char character) {
            return static_cast<char>(std::tolower(character));
        });
        if (extensions.contains(extension)) {
            return true;
        }
    }
    return false;
}

json discovery_rows(const SetupPaths& paths, const std::vector<fs::path>& extra_roots) {
    json rows = json::object();
    for (const auto kind : discovery_kinds) {
        rows[std::string(kind)] = {
            {"schema", "arena_setup_discovery_v1"},
            {"schema_version", 1},
            {"kind", kind},
            {"precedence",
             {"install_receipt", "saved_locator", "provider_manifest", "discovered", "unresolved"}},
            {"selection", nullptr},
            {"candidates", json::array()},
            {"browse", browse_contract(kind)},
        };
    }
    std::map<std::string, std::set<std::string>> seen;

    for (const auto& receipt : receipt_rows(paths)) {
        const auto kind = discovery_kind_for_component(receipt.value("component_id", ""));
        if (!kind || (receipt.value("state", "") != "installed_managed" &&
                      receipt.value("state", "") != "installed_disabled")) {
            continue;
        }
        const fs::path locator = receipt_locator(receipt);
        const bool healthy =
            !locator.empty() && fs::exists(locator) && !contains_symbolic_link(locator);
        append_discovery_candidate(rows, seen, *kind, "install_receipt", locator,
                                   healthy ? receipt.value("state", "installed_managed") : "broken",
                                   {{"component_id", receipt.value("component_id", "")},
                                    {"component_version", receipt.value("component_version", "")}});
    }

    const fs::path locator_root = paths.data_root / "locators";
    if (fs::is_directory(locator_root) && !contains_symbolic_link(locator_root)) {
        for (const auto kind : discovery_kinds) {
            const fs::path locator_path = locator_root / (std::string(kind) + ".json");
            if (!fs::is_regular_file(locator_path) || contains_symbolic_link(locator_path)) {
                continue;
            }
            try {
                const json locator = read_json(locator_path);
                if (locator.value("schema", "") != "arena_saved_locator_v1" ||
                    locator.value("schema_version", 0) != 1 || locator.value("kind", "") != kind ||
                    !locator.contains("path") || !locator.at("path").is_string()) {
                    continue;
                }
                const fs::path selected = locator.at("path").get<std::string>();
                const bool healthy = fs::exists(selected) && !contains_symbolic_link(selected);
                append_discovery_candidate(rows, seen, std::string(kind), "saved_locator", selected,
                                           healthy ? "saved" : "broken",
                                           {{"mode", locator.value("mode", "in_place_reference")}});
            } catch (const std::exception&) {
            }
        }
    }

    std::vector<fs::path> bounded_roots{
        paths.install_root,
        paths.workspace_root,
        paths.knowledge_root,
    };
    bounded_roots.insert(bounded_roots.end(), extra_roots.begin(), extra_roots.end());
    inspect_provider_roots(rows, seen, bounded_roots);

    const fs::path book_root = paths.knowledge_root / "opening_books";
    if (directory_has_extension(book_root, {".bin", ".book"})) {
        append_discovery_candidate(rows, seen, "opening_book", "discovered", book_root,
                                   "detected_unmanaged");
    }
    const fs::path tablebase_root = paths.knowledge_root / "tablebases";
    if (directory_has_extension(tablebase_root, {".rtbw", ".rtbz"})) {
        append_discovery_candidate(rows, seen, "syzygy", "discovered", tablebase_root,
                                   "detected_unmanaged");
    }

    const auto selected_roots = bounded_root_candidates(extra_roots);
    for (const auto& selected : selected_roots) {
        if (fs::is_regular_file(selected)) {
            std::string extension = selected.extension().string();
            std::ranges::transform(extension, extension.begin(), [](const unsigned char character) {
                return static_cast<char>(std::tolower(character));
            });
            if (extension == ".bin" || extension == ".book") {
                append_discovery_candidate(rows, seen, "opening_book", "discovered", selected,
                                           "detected_unmanaged");
            }
            continue;
        }
        if (directory_has_extension(selected, {".bin", ".book"})) {
            append_discovery_candidate(rows, seen, "opening_book", "discovered", selected,
                                       "detected_unmanaged");
        }
        if (directory_has_extension(selected, {".rtbw", ".rtbz"})) {
            append_discovery_candidate(rows, seen, "syzygy", "discovered", selected,
                                       "detected_unmanaged");
        }
    }

    if (const char* path_environment = std::getenv("PATH")) {
#ifdef _WIN32
        constexpr char separator = ';';
#else
        constexpr char separator = ':';
#endif
        std::stringstream stream(path_environment);
        for (std::string item; std::getline(stream, item, separator);) {
            if (!item.empty()) {
                bounded_roots.emplace_back(item);
            }
        }
    }
    const std::set<std::string> engine_names{
#ifdef _WIN32
        "uci_arena_engine.exe", "stockfish.exe", "lc0.exe"
#else
        "uci_arena_engine", "stockfish", "lc0"
#endif
    };
    std::set<std::string> engine_roots;
    for (const auto& root : bounded_root_candidates(bounded_roots)) {
        if (fs::is_regular_file(root)) {
            if (engine_names.contains(root.filename().string())) {
                append_discovery_candidate(rows, seen, "uci_engine", "discovered", root,
                                           "detected_unmanaged", {{"requires_probe", true}});
            }
            continue;
        }
        if (!engine_roots.insert(root.string()).second || !fs::is_directory(root)) {
            continue;
        }
        for (const auto& name : engine_names) {
            const fs::path executable = root / name;
            if (fs::is_regular_file(executable) && !contains_symbolic_link(executable)) {
                append_discovery_candidate(rows, seen, "uci_engine", "discovered", executable,
                                           "detected_unmanaged", {{"requires_probe", true}});
            }
        }
    }

    const std::array<std::tuple<const char*, const char*, const char*>, 7> environment_hints{{
        {"BOOK_FORGE_CLI", "book_forge", "provider_cli"},
        {"UCI_ARENA_EVIDENCE_CLI", "evidence_service", "provider_cli"},
        {"UCI_ARENA_SINGLE_GAME_PROFILE", "lichess_single_game_profile",
         "qualified_profile"},
        {"UCI_ARENA_LICHESS_CAPACITY_PROFILE", "lichess_capacity_profile",
         "qualified_profile"},
        {"UCI_ARENA_LICHESS_TRANSPORT_PROFILE", "lichess_transport_profile",
         "qualified_profile"},
        {"LICHESS_BOT_TOKEN_PATH", "lichess_token_path", "token_path_reference"},
        {"RESTAURANT_ROOT", "restaurant", "detect_only"},
    }};
    for (const auto& [name, kind, hint] : environment_hints) {
        const char* value = std::getenv(name);
        if (value == nullptr || *value == '\0') {
            continue;
        }
        const fs::path path = value;
        if (!fs::exists(path) || contains_symbolic_link(path)) {
            continue;
        }
        append_discovery_candidate(rows, seen, kind, "discovered", path, "detected_unmanaged",
                                   {{"hint", hint}});
    }

    for (const auto kind : discovery_kinds) {
        if (rows.at(std::string(kind)).at("selection").is_null()) {
            rows.at(std::string(kind))["selection"] = {
                {"source", "unresolved"},
                {"state", "unresolved"},
                {"path", nullptr},
            };
        }
    }
    return rows;
}

json validate_provider_locator(const fs::path& path, const std::string& expected_component_id) {
    if (fs::is_regular_file(path)) {
        return {
            {"validation", "entrypoint_present"},
            {"entrypoint", path.string()},
        };
    }
    if (!fs::is_directory(path)) {
        throw std::runtime_error(
            "provider locator must be a regular CLI entrypoint or provider root");
    }
    const fs::path manifest_path = path / "arena-component.json";
    if (!fs::is_regular_file(manifest_path) || contains_symbolic_link(manifest_path)) {
        throw std::runtime_error("provider root does not contain a safe arena-component.json");
    }
    std::error_code error;
    const auto size = fs::file_size(manifest_path, error);
    if (error || size > 1024ULL * 1024ULL) {
        throw std::runtime_error("provider manifest exceeds the bounded size");
    }
    const json manifest = read_json(manifest_path);
    if (manifest.value("schema", "") != "arena_install_component_v1" ||
        manifest.value("component_id", "") != expected_component_id) {
        throw std::runtime_error("provider manifest has an incompatible component identity");
    }
    return {
        {"validation", "provider_manifest"},
        {"component_id", expected_component_id},
        {"component_version", manifest.value("component_version", "")},
    };
}

json validate_book_locator(const fs::path& path) {
    if (fs::is_regular_file(path)) {
        return {
            {"validation", "regular_file"},
            {"file_name", path.filename().string()},
        };
    }
    if (!fs::is_directory(path)) {
        throw std::runtime_error("opening book locator must be a regular file or flat directory");
    }
    std::uint64_t file_count = 0;
    bool has_book = false;
    for (const auto& entry : fs::directory_iterator(path)) {
        if (entry.is_symlink()) {
            throw std::runtime_error("opening book directory contains a symbolic link");
        }
        if (!entry.is_regular_file()) {
            throw std::runtime_error(
                "opening book directory must be flat and contain only regular files");
        }
        ++file_count;
        std::string extension = entry.path().extension().string();
        std::ranges::transform(extension, extension.begin(), [](const unsigned char character) {
            return static_cast<char>(std::tolower(character));
        });
        has_book = has_book || extension == ".bin" || extension == ".book";
    }
    if (file_count == 0 || !has_book) {
        throw std::runtime_error("opening book directory contains no supported book file");
    }
    return {
        {"validation", "flat_directory"},
        {"file_count", file_count},
    };
}

json validate_syzygy_locator(const fs::path& path) {
    if (!fs::is_directory(path)) {
        throw std::runtime_error("Syzygy locator must be a directory");
    }
    std::uint64_t wdl_files = 0;
    std::uint64_t dtz_files = 0;
    std::uint64_t bytes = 0;
    for (const auto& entry : fs::directory_iterator(path)) {
        if (entry.is_symlink()) {
            throw std::runtime_error("Syzygy directory contains a symbolic link");
        }
        if (!entry.is_regular_file()) {
            continue;
        }
        std::string extension = entry.path().extension().string();
        std::ranges::transform(extension, extension.begin(), [](const unsigned char character) {
            return static_cast<char>(std::tolower(character));
        });
        if (extension == ".rtbw") {
            ++wdl_files;
        } else if (extension == ".rtbz") {
            ++dtz_files;
        } else {
            continue;
        }
        bytes += entry.file_size();
    }
    if (wdl_files == 0 && dtz_files == 0) {
        throw std::runtime_error("Syzygy directory contains no .rtbw or .rtbz files");
    }
    return {
        {"validation", "syzygy_directory"},
        {"wdl_files", wdl_files},
        {"dtz_files", dtz_files},
        {"bytes", bytes},
    };
}

json validate_uci_locator(const fs::path& path) {
    if (!fs::is_regular_file(path)) {
        throw std::runtime_error("UCI engine locator must be a regular file");
    }
    const auto probe = adapters::UciProcess::probe(path, path.parent_path(), {});
    if (!probe.ok) {
        throw std::runtime_error("UCI engine probe failed: " + probe.error);
    }
    return {
        {"validation", "uci_startup_interrogation"},
        {"engine_name", probe.engine_name},
        {"engine_author", probe.engine_author},
        {"fingerprint", probe.fingerprint},
        {"option_count", probe.options.size()},
        {"bestmove", probe.bestmove},
        {"elapsed_ms", probe.elapsed_ms},
    };
}

json validate_timing_profile_locator(const fs::path& path) {
    if (!fs::is_regular_file(path)) {
        throw std::runtime_error("timing profile locator must reference a regular file");
    }
    std::error_code error;
    const auto size = fs::file_size(path, error);
    if (error || size == 0 || size > 16ULL * 1024ULL * 1024ULL) {
        throw std::runtime_error("timing profile must be non-empty and no larger than 16 MiB");
    }
    const json profile = read_json(path);
    if (profile.value("schema", "") != "uci_time_profile_v3" ||
        profile.value("qualified", false) != true ||
        profile.value("gates", json::object()).value("passed", false) != true) {
        throw std::runtime_error("timing profile is not a qualified uci_time_profile_v3");
    }
    const json permissions = profile.value("permissions", json::object());
    for (const auto* field :
         {"provider_safety_qualified", "forecast_shadow_qualified", "adaptive_play_qualified"}) {
        if (!permissions.contains(field) || !permissions.at(field).is_boolean()) {
            throw std::runtime_error("timing profile permissions are incomplete");
        }
    }
    if (!permissions.at("provider_safety_qualified").get<bool>()) {
        throw std::runtime_error("timing profile lacks provider-safety qualification");
    }
    const json identity = profile.value("identity", json::object());
    const std::string identity_sha256 = profile.value("identity_sha256", "");
    const std::string expected_identity_sha256 = "sha256:" + sha256_text(identity.dump());
    if (!identity.is_object() || identity_sha256 != expected_identity_sha256) {
        throw std::runtime_error("timing profile identity digest is missing or invalid");
    }
    const std::string model_id = identity.value("model", json::object()).value("model_id", "");
    const std::string model_fingerprint =
        identity.value("model", json::object()).value("model_fingerprint", "");
    const std::string engine_sha256 =
        identity.value("engine", json::object()).value("executable_sha256", "");
    const std::string provider =
        identity.value("search", json::object()).value("provider_effective", "");
    if (!valid_identifier(model_id) || model_fingerprint.empty() || engine_sha256.size() != 71 ||
        !engine_sha256.starts_with("sha256:") || provider.empty()) {
        throw std::runtime_error("timing profile exact runtime identity is incomplete");
    }
    return {
        {"validation", "qualified_uci_time_profile_v3"},
        {"sha256", sha256_file(path)},
        {"identity_sha256", identity_sha256},
        {"model_id", model_id},
        {"model_fingerprint", model_fingerprint},
        {"engine_sha256", engine_sha256},
        {"provider", provider},
        {"permissions", permissions},
    };
}

json validate_qualified_profile_locator(const fs::path& path, const std::string_view schema,
                                        const std::string_view label) {
    if (!fs::is_regular_file(path)) {
        throw std::runtime_error(std::string(label) +
                                 " locator must reference a regular file");
    }
    std::error_code error;
    const auto size = fs::file_size(path, error);
    if (error || size == 0 || size > 16ULL * 1024ULL * 1024ULL) {
        throw std::runtime_error(std::string(label) +
                                 " must be non-empty and no larger than 16 MiB");
    }
    const json profile = read_json(path);
    if (profile.value("schema", "") != schema ||
        !profile.value("qualified", false)) {
        throw std::runtime_error(std::string(label) + " is not qualified");
    }
    return {
        {"validation", "qualified_" + std::string(schema)},
        {"sha256", sha256_file(path)},
    };
}

struct ManagedProfileFile {
    fs::path source;
    fs::path relative;
    std::uint64_t size{};
    std::string sha256;
};

std::vector<ManagedProfileFile> lichess_single_game_profile_files(const fs::path& profile_path) {
    const json profile = read_json(profile_path);
    if (profile.value("schema", "") != "uci_arena_single_game_profile_v1" ||
        !profile.value("qualified", false)) {
        throw std::runtime_error("Lichess single-game profile is not qualified");
    }
    const json evidence = profile.value("evidence", json::object());
    static constexpr std::array<std::string_view, 5> required_evidence{
        "cursor_correctness", "provider_parity", "deadline", "topology", "complete_game"};
    std::vector<ManagedProfileFile> files;
    const auto profile_size = static_cast<std::uint64_t>(fs::file_size(profile_path));
    if (profile_size == 0 || profile_size > managed_profile_max_bytes) {
        throw std::runtime_error("Lichess single-game profile exceeds the managed closure limit");
    }
    files.push_back({profile_path, profile_path.filename(), profile_size,
                     sha256_file(profile_path)});
    std::set<std::string> relative_paths{profile_path.filename().generic_string()};
    std::uint64_t total = profile_size;

    for (const auto name : required_evidence) {
        if (!evidence.contains(name) || !evidence.at(name).is_object()) {
            throw std::runtime_error("Lichess single-game profile evidence " +
                                     std::string(name) + " is missing");
        }
        const json& record = evidence.at(name);
        if (!record.value("passed", false) || !record.contains("artifact") ||
            !record.at("artifact").is_string() || !record.contains("artifact_sha256") ||
            !record.at("artifact_sha256").is_string()) {
            throw std::runtime_error("Lichess single-game profile evidence " +
                                     std::string(name) + " is incomplete");
        }
        const fs::path relative = fs::path(record.at("artifact").get<std::string>())
                                      .lexically_normal();
        if (!safe_relative_path(relative)) {
            throw std::runtime_error("Lichess single-game profile evidence " +
                                     std::string(name) + " has an unsafe relative path");
        }
        const std::string expected = record.at("artifact_sha256").get<std::string>();
        if (expected.size() != 71 || !expected.starts_with("sha256:")) {
            throw std::runtime_error("Lichess single-game profile evidence " +
                                     std::string(name) + " has an invalid digest");
        }
        if (relative_paths.contains(relative.generic_string())) {
            continue;
        }

        std::optional<fs::path> match;
        fs::path root = profile_path.parent_path();
        for (int depth = 0; depth < 8 && !root.empty(); ++depth) {
            const fs::path candidate = normalized_absolute(root / relative);
            if (within(root, candidate) && fs::is_regular_file(candidate) &&
                !contains_symbolic_link(candidate) &&
                "sha256:" + sha256_file(candidate) == expected) {
                match = candidate;
                break;
            }
            const fs::path parent = root.parent_path();
            if (parent == root) {
                break;
            }
            root = parent;
        }
        if (!match.has_value()) {
            throw std::runtime_error("Lichess single-game profile evidence " +
                                     std::string(name) +
                                     " is missing or does not match its declared digest");
        }
        const auto size = static_cast<std::uint64_t>(fs::file_size(*match));
        if (files.size() >= managed_profile_max_files ||
            size > managed_profile_max_bytes - total) {
            throw std::runtime_error(
                "Lichess single-game profile evidence closure exceeds its managed limit");
        }
        files.push_back({*match, relative, size, expected.substr(7)});
        relative_paths.insert(relative.generic_string());
        total += size;
    }
    return files;
}

json validate_import_locator(const std::string& kind, const fs::path& path) {
    if (!valid_discovery_kind(kind)) {
        throw std::runtime_error("unknown import kind " + kind);
    }
    if (kind == "restaurant") {
        throw std::runtime_error("Restaurant is detect-only and cannot be imported");
    }
    if (!fs::exists(path)) {
        throw std::runtime_error("import path does not exist");
    }
    require_no_symbolic_link(path);
    if (kind == "opening_book") {
        return validate_book_locator(path);
    }
    if (kind == "syzygy") {
        return validate_syzygy_locator(path);
    }
    if (kind == "uci_engine") {
        return validate_uci_locator(path);
    }
    if (kind == "book_forge") {
        return validate_provider_locator(path, "book_forge.service");
    }
    if (kind == "evidence_service") {
        return validate_provider_locator(path, "uci_arena_evidence.service");
    }
    if (kind == "timing_profile") {
        return validate_timing_profile_locator(path);
    }
    if (kind == "lichess_single_game_profile") {
        const auto files = lichess_single_game_profile_files(path);
        std::uint64_t total = 0;
        for (const auto& file : files) {
            total += file.size;
        }
        return {
            {"validation", "qualified_uci_arena_single_game_profile_v1"},
            {"sha256", sha256_file(path)},
            {"evidence_files", files.size() - 1},
            {"closure_bytes", total},
        };
    }
    if (kind == "lichess_capacity_profile") {
        return validate_qualified_profile_locator(
            path, "uci_arena_lichess_capacity_profile_v1", "Lichess capacity profile");
    }
    if (kind == "lichess_transport_profile") {
        return validate_qualified_profile_locator(
            path, "uci_arena_lichess_transport_profile_v2", "Lichess transport profile");
    }
    if (kind == "lichess_token_path") {
        if (!fs::is_regular_file(path)) {
            throw std::runtime_error("Lichess token locator must reference a regular file");
        }
        return {{"validation", "path_reference_only"}};
    }
    throw std::runtime_error("unsupported import kind " + kind);
}

struct ManagedBookFile {
    fs::path source;
    fs::path relative;
    std::uint64_t size{};
    std::string sha256;
};

std::vector<ManagedBookFile> managed_book_files(const fs::path& source) {
    std::vector<ManagedBookFile> files;
    std::uint64_t total = 0;
    if (fs::is_regular_file(source)) {
        const auto size = static_cast<std::uint64_t>(fs::file_size(source));
        if (size > managed_book_max_bytes) {
            throw std::runtime_error("managed opening-book copy exceeds the 256 MiB limit");
        }
        files.push_back({source, source.filename(), size, sha256_file(source)});
        total = size;
    } else {
        for (const auto& entry : fs::directory_iterator(source)) {
            if (entry.is_symlink()) {
                throw std::runtime_error(
                    "managed opening-book copy cannot traverse a symbolic link");
            }
            if (!entry.is_regular_file()) {
                throw std::runtime_error("managed opening-book copy requires a flat directory");
            }
            if (files.size() >= managed_book_max_files) {
                throw std::runtime_error("managed opening-book copy exceeds the 64-file limit");
            }
            const auto size = static_cast<std::uint64_t>(entry.file_size());
            if (size > managed_book_max_bytes - total) {
                throw std::runtime_error("managed opening-book copy exceeds the 256 MiB limit");
            }
            files.push_back(
                {entry.path(), entry.path().filename(), size, sha256_file(entry.path())});
            total += size;
        }
    }
    std::ranges::sort(files, {},
                      [](const ManagedBookFile& file) { return file.relative.generic_string(); });
    for (const auto& file : files) {
        if (!safe_relative_path(file.relative)) {
            throw std::runtime_error("managed opening-book copy has an unsafe file name");
        }
    }
    if (files.empty()) {
        throw std::runtime_error("managed opening-book copy has no files");
    }
    return files;
}

json copy_managed_book(const SetupPaths& paths, const fs::path& source, const json& validation) {
    const auto files = managed_book_files(source);
    json identity_rows = json::array();
    for (const auto& file : files) {
        identity_rows.push_back({{"path", file.relative.generic_string()},
                                 {"size_bytes", file.size},
                                 {"sha256", file.sha256}});
    }
    const std::string fingerprint = sha256_text(identity_rows.dump());
    const std::string component_id = "opening_book.import." + fingerprint.substr(0, 16);
    const std::string component_version = "1";
    const fs::path import_root = paths.knowledge_root / "imports" / "opening_books";
    const fs::path active_root = import_root / fingerprint;
    const fs::path staging_root = import_root / (".staging-" + fingerprint);
    const bool active_root_existed = fs::exists(active_root);
    fs::create_directories(import_root);
    require_no_symbolic_link(import_root);

    std::error_code ignored;
    fs::remove_all(staging_root, ignored);
    if (!fs::exists(active_root)) {
        fs::create_directories(staging_root);
        try {
            for (const auto& file : files) {
                const fs::path target = staging_root / file.relative;
                if (!within(staging_root, target)) {
                    throw std::runtime_error("managed opening-book path escapes staging");
                }
                fs::copy_file(file.source, target, fs::copy_options::overwrite_existing);
                if (fs::file_size(target) != file.size || sha256_file(target) != file.sha256) {
                    throw std::runtime_error("managed opening-book copy verification failed");
                }
            }
            const json manifest{
                {"schema", "arena_install_component_v1"},
                {"schema_version", 1},
                {"component_id", component_id},
                {"component_version", component_version},
                {"files", identity_rows},
            };
            write_json_atomic(staging_root / "arena-component.json", manifest);
            fs::rename(staging_root, active_root);
        } catch (...) {
            fs::remove_all(staging_root, ignored);
            throw;
        }
    } else {
        require_no_symbolic_link(active_root);
        validate_component_payload(active_root, component_id, component_version);
    }

    json activated_files = json::array();
    for (const auto& file : files) {
        activated_files.push_back({{"path", (active_root / file.relative).string()},
                                   {"size_bytes", file.size},
                                   {"sha256", file.sha256}});
    }
    const json receipt{
        {"schema", "arena_install_receipt_v1"}, {"schema_version", 1},
        {"component_id", component_id},         {"component_version", component_version},
        {"state", "installed_managed"},         {"active_root", active_root.string()},
        {"locator_path", active_root.string()}, {"artifact_sha256", fingerprint},
        {"import_mode", "managed_copy"},        {"activated_files", activated_files},
        {"installed_at", current_utc_iso()},
    };
    const fs::path receipt_path = paths.data_root / "receipts" / (component_id + ".json");
    const bool receipt_existed = fs::is_regular_file(receipt_path);
    write_json_atomic(receipt_path, receipt);
    return {
        {"schema", "arena_setup_import_result_v1"},
        {"schema_version", 1},
        {"kind", "opening_book"},
        {"mode", "managed_copy"},
        {"state", "installed_managed"},
        {"path", active_root.string()},
        {"validation", validation},
        {"receipt", receipt},
        {"_active_root_existed", active_root_existed},
        {"_receipt_existed", receipt_existed},
    };
}

json managed_knowledge_components(const SetupPaths& paths,
                                  const std::string_view component_prefix) {
    json components = json::array();
    for (const auto& receipt : receipt_rows(paths)) {
        const std::string component_id = receipt.value("component_id", "");
        if (receipt.value("schema", "") != "arena_install_receipt_v1" ||
            receipt.value("state", "") != "installed_managed" ||
            component_id.rfind(component_prefix, 0) != 0 || !receipt.contains("activated_files") ||
            !receipt.at("activated_files").is_array() || receipt.at("activated_files").empty()) {
            continue;
        }
        bool present = true;
        for (const auto& activated : receipt.at("activated_files")) {
            if (!activated.contains("path") || !activated.at("path").is_string() ||
                !activated.contains("size_bytes") ||
                !activated.at("size_bytes").is_number_unsigned()) {
                present = false;
                break;
            }
            const fs::path path = activated.at("path").get<std::string>();
            if (!within(paths.knowledge_root, path) || !fs::is_regular_file(path) ||
                fs::is_symlink(path) ||
                fs::file_size(path) != integer_field(activated, "size_bytes")) {
                present = false;
                break;
            }
        }
        if (present) {
            components.push_back(component_id);
        }
    }
    return components;
}

fs::path installed_entrypoint(const fs::path& receipts_root, const std::string& component_id,
                              const std::string& entrypoint_name) {
    const json receipt = read_json(receipts_root / (component_id + ".json"));
    if (receipt.value("schema", "") != "arena_install_receipt_v1" ||
        receipt.value("component_id", "") != component_id ||
        receipt.value("state", "") != "installed_managed") {
        throw std::runtime_error("required installed dependency is unavailable: " + component_id);
    }
    const fs::path active_root = receipt.at("active_root").get<std::string>();
    const json manifest = read_json(active_root / "arena-component.json");
    if (manifest.value("schema", "") != "arena_install_component_v1" ||
        manifest.value("component_id", "") != component_id || !manifest.contains("entrypoints") ||
        !manifest.at("entrypoints").contains(entrypoint_name)) {
        throw std::runtime_error("required installed dependency entrypoint is unavailable: " +
                                 component_id + ":" + entrypoint_name);
    }
    const fs::path relative = manifest.at("entrypoints").at(entrypoint_name).get<std::string>();
    if (!safe_relative_path(relative)) {
        throw std::runtime_error("installed dependency entrypoint is unsafe");
    }
    const fs::path entrypoint = active_root / relative;
    for (const auto& file : manifest.at("files")) {
        if (file.value("path", "") == relative.generic_string() &&
            fs::is_regular_file(entrypoint) && !fs::is_symlink(entrypoint) &&
            fs::file_size(entrypoint) == integer_field(file, "size_bytes") &&
            sha256_file(entrypoint) == file.at("sha256").get<std::string>()) {
            return normalized_absolute(entrypoint);
        }
    }
    throw std::runtime_error("installed dependency entrypoint digest mismatch: " + component_id +
                             ":" + entrypoint_name);
}

fs::path installed_active_root(const fs::path& receipts_root, const std::string& component_id) {
    const json receipt = read_json(receipts_root / (component_id + ".json"));
    if (receipt.value("schema", "") != "arena_install_receipt_v1" ||
        receipt.value("component_id", "") != component_id ||
        receipt.value("state", "") != "installed_managed" || !receipt.contains("active_root") ||
        !receipt.at("active_root").is_string()) {
        throw std::runtime_error("required installed dependency is unavailable: " + component_id);
    }
    const fs::path root = normalized_absolute(receipt.at("active_root").get<std::string>());
    if (!fs::is_directory(root) || contains_symbolic_link(root)) {
        throw std::runtime_error("required installed dependency root is unavailable: " +
                                 component_id);
    }
    return root;
}

std::optional<fs::path> saved_locator_path(const fs::path& data_root, const std::string& kind) {
    const fs::path locator_path = data_root / "locators" / (kind + ".json");
    if (!fs::is_regular_file(locator_path) || contains_symbolic_link(locator_path)) {
        return std::nullopt;
    }
    const json locator = read_json(locator_path);
    const std::string mode = locator.value("mode", "");
    if (locator.value("schema", "") != "arena_saved_locator_v1" ||
        locator.value("schema_version", 0) != 1 || locator.value("kind", "") != kind ||
        (mode != "in_place_reference" &&
         !(kind == "lichess_single_game_profile" && mode == "managed_copy")) ||
        !locator.contains("path") ||
        !locator.at("path").is_string()) {
        throw std::runtime_error("saved " + kind + " locator is invalid");
    }
    const fs::path path = normalized_absolute(locator.at("path").get<std::string>());
    static_cast<void>(validate_import_locator(kind, path));
    return path;
}

json timing_profile_uci_options(const json& profile) {
    const json search = profile.at("identity").at("search");
    json options = search.value("extra_engine_options", json::object());
    if (!options.is_object()) {
        throw std::runtime_error("timing profile extra engine options are invalid");
    }
    options["SearchCore"] = search.at("search_core");
    options["Threads"] = search.at("threads");
    options["RootWorkers"] = search.at("root_workers");
    options["V2SplitDepth"] = search.at("split_depth");
    options["BackgroundSearch"] = search.at("background_search");
    options["NNSpeculation"] = search.at("nn_speculation");
    options["NNBatchSize"] = search.at("batch_size_effective");
    options["NNBatchWaitUs"] = search.at("batch_wait_us_effective");
    options["OnnxProvider"] = search.at("provider_effective");
    return options;
}

std::string installed_engine_sha256(const fs::path& engine) {
    if (!fs::is_regular_file(engine) || contains_symbolic_link(engine)) {
        throw std::runtime_error("installed UCI Arena engine is unavailable");
    }
    return "sha256:" + sha256_file(engine);
}

json installed_checkpoint_identity(const fs::path& model_root) {
    const fs::path manifest_path = model_root / "checkpoint_manifest.json";
    if (!fs::is_regular_file(manifest_path) || contains_symbolic_link(manifest_path)) {
        throw std::runtime_error("installed UCI Arena checkpoint manifest is unavailable");
    }
    const json manifest = read_json(manifest_path);
    const std::string model_id =
        manifest.value("model_id", manifest.value("active_model_id", ""));
    const std::string checkpoint_hash = manifest.value("checkpoint_hash", "");
    if (!valid_identifier(model_id) || checkpoint_hash.size() != 64) {
        throw std::runtime_error("installed UCI Arena checkpoint identity is incomplete");
    }
    return {{"model_id", model_id}, {"checkpoint_hash", checkpoint_hash}};
}

void require_exact_timing_profile(const json& profile, const std::string& engine_sha256,
                                  const json& checkpoint) {
    const json identity = profile.value("identity", json::object());
    if (profile.value("schema", "") != "uci_time_profile_v3" ||
        !profile.value("qualified", false) ||
        identity.value("engine", json::object()).value("executable_sha256", "") !=
            engine_sha256 ||
        identity.value("model", json::object()).value("model_id", "") !=
            checkpoint.at("model_id").get<std::string>()) {
        throw std::runtime_error(
            "selected timing profile does not match the exact installed engine and model");
    }
}

void require_exact_lichess_profiles(const fs::path& timing_path,
                                    const fs::path& single_path,
                                    const fs::path& capacity_path,
                                    const std::string& engine_sha256,
                                    const json& checkpoint,
                                    const std::uint64_t configured_capacity) {
    const json timing = read_json(timing_path);
    require_exact_timing_profile(timing, engine_sha256, checkpoint);
    const json single = read_json(single_path);
    if (single.value("schema", "") != "uci_arena_single_game_profile_v1" ||
        !single.value("qualified", false) ||
        single.value("engine_build_fingerprint", "") != engine_sha256 ||
        single.value("model_id", "") != checkpoint.at("model_id").get<std::string>() ||
        single.value("checkpoint_hash", "") !=
            checkpoint.at("checkpoint_hash").get<std::string>()) {
        throw std::runtime_error(
            "selected Lichess single-game profile does not match the exact installed "
            "engine and model");
    }
    const json timing_model = timing.at("identity").at("model");
    const json timing_provider = timing.at("identity").at("provider");
    if (single.value("model_fingerprint", "") !=
            timing_model.value("model_fingerprint", "") ||
        single.value("adapter_id", "") != timing_model.value("adapter_id", "") ||
        single.value("provider", "") !=
            timing.at("identity").at("search").value("provider_effective", "") ||
        single.value("provider_numeric_fingerprint", "") !=
            timing_provider.value("provider_numeric_fingerprint", "") ||
        single.value("hardware_driver_fingerprint", "") !=
            timing_provider.value("hardware_driver_fingerprint", "") ||
        single.value("onnx_runtime_version", "") !=
            timing_provider.value("onnx_runtime_version", "")) {
        throw std::runtime_error(
            "selected timing and Lichess single-game profiles have different runtime "
            "identities");
    }
    const json capacity = read_json(capacity_path);
    if (capacity.value("schema", "") !=
            "uci_arena_lichess_capacity_profile_v1" ||
        !capacity.value("qualified", false) ||
        capacity.value("engine_build_fingerprint", "") != engine_sha256 ||
        capacity.value("single_game_profile_sha256", "") !=
            "sha256:" + sha256_file(single_path) ||
        capacity.value("model_id", "") != single.value("model_id", "") ||
        capacity.value("model_fingerprint", "") !=
            single.value("model_fingerprint", "") ||
        capacity.value("provider", "") != single.value("provider", "") ||
        capacity.value("provider_numeric_fingerprint", "") !=
            single.value("provider_numeric_fingerprint", "") ||
        capacity.value("hardware_driver_fingerprint", "") !=
            single.value("hardware_driver_fingerprint", "") ||
        capacity.value("maximum_concurrency", 0ULL) < configured_capacity) {
        throw std::runtime_error(
            "selected Lichess capacity profile does not match the exact single-game "
            "profile and configured capacity");
    }
}

json installed_engine_locator_options(const fs::path& data_root) {
    json options = json::object();
    if (const auto syzygy = saved_locator_path(data_root, "syzygy"); syzygy.has_value()) {
        options["SyzygyPath"] = syzygy->string();
    }
    const auto opening_book = saved_locator_path(data_root, "opening_book");
    if (!opening_book.has_value()) {
        return options;
    }

    fs::path book_file;
    if (fs::is_regular_file(*opening_book)) {
        book_file = *opening_book;
    } else {
        const fs::path canonical = *opening_book / "strong_rare_v1.bin";
        if (fs::is_regular_file(canonical) && !fs::is_symlink(canonical)) {
            book_file = canonical;
        } else {
            std::vector<fs::path> candidates;
            for (const auto& entry : fs::directory_iterator(*opening_book)) {
                if (!entry.is_regular_file() || entry.is_symlink()) {
                    continue;
                }
                std::string extension = entry.path().extension().string();
                std::ranges::transform(extension, extension.begin(),
                                       [](const unsigned char character) {
                                           return static_cast<char>(std::tolower(character));
                                       });
                if (extension == ".bin" || extension == ".book") {
                    candidates.push_back(entry.path());
                }
            }
            std::ranges::sort(candidates);
            if (candidates.size() != 1) {
                throw std::runtime_error("opening-book directory is ambiguous; import the intended "
                                         "book file explicitly");
            }
            book_file = candidates.front();
        }
    }
    options["OwnBook"] = true;
    options["BookFile"] = normalized_absolute(book_file).string();
    const fs::path stats = book_file.parent_path() / (book_file.stem().string() + ".ffstats");
    if (fs::is_regular_file(stats) && !fs::is_symlink(stats)) {
        options["BookStatsFile"] = normalized_absolute(stats).string();
    }
    const fs::path policy = book_file.parent_path() / (book_file.stem().string() + ".policy");
    if (fs::is_regular_file(policy) && !fs::is_symlink(policy)) {
        options["BookPolicyFile"] = normalized_absolute(policy).string();
    }
    return options;
}

json generated_component_config(const std::string& component_id, const fs::path& receipts_root,
                                const fs::path& workspace, const fs::path& component_root) {
    const fs::path data_root = receipts_root.parent_path();
    if (component_id == "book_forge.service") {
        json config{
            {"schema", "uci_arena_book_forge_config_v1"},
            {"stockfish",
             installed_entrypoint(receipts_root, "stockfish.engine", "stockfish").string()},
        };
        if (const auto token_path = saved_locator_path(data_root, "lichess_token_path");
            token_path.has_value()) {
            config["acquisition"] = {{"tokenFile", token_path->string()}};
        }
        return config;
    }
    if (component_id == "uci_arena_evidence.service") {
        const fs::path engine = installed_entrypoint(receipts_root, "uci_arena.engine", "uci");
        const fs::path model_root =
            installed_active_root(receipts_root, "uci_arena.engine") / "models" / "default";
        if (!fs::is_directory(model_root) || contains_symbolic_link(model_root)) {
            throw std::runtime_error("installed UCI Arena model root is unavailable");
        }
        const std::string engine_sha256 = installed_engine_sha256(engine);
        json config{
            {"schema", "uci_arena_evidence_service_config_v1"},
            {"workspace", workspace.string()},
            {"activeModelPointer",
             (workspace / "runtime-models" / "active_model_runtime.json").string()},
            {"runtimeModelsRoot", (workspace / "runtime-models").string()},
            {"profileImports", json::array()},
            {"engine", engine.string()},
            {"stockfish",
             installed_entrypoint(receipts_root, "stockfish.engine", "stockfish").string()},
            {"referee",
             installed_entrypoint(receipts_root, "uci_arena.engine", "referee").string()},
        };
        if (const auto profile_path = saved_locator_path(data_root, "timing_profile");
            profile_path.has_value()) {
            const json profile = read_json(*profile_path);
            const json identity = profile.at("identity");
            config["profileImports"].push_back(profile_path->string());
            config["model_id"] = identity.at("model").at("model_id");
            config["model_fingerprint"] = identity.at("model").at("model_fingerprint");
            config["model_root"] = model_root.string();
            config["provider"] = identity.at("search").at("provider_effective");
            config["hardware_driver_fingerprint"] =
                identity.at("provider").value("hardware_driver_fingerprint", "");
            config["engine_path"] = engine.string();
            config["engine_sha256"] = engine_sha256;
            config["uci_options"] = timing_profile_uci_options(profile);
        }
        return config;
    }
    if (component_id == "lichess.adapter") {
        const fs::path template_path =
            component_root / "deploy" / "lichess" / "config.uci_arena.template.yml";
        json config = read_json(template_path);
        if (config.value("schema", "") != "uci_arena_lichess_adapter_v5") {
            throw std::runtime_error("installed Lichess adapter template is incompatible");
        }
        const fs::path engine = installed_entrypoint(receipts_root, "uci_arena.engine", "uci");
        const fs::path engine_root = installed_active_root(receipts_root, "uci_arena.engine");
        const fs::path model_root = engine_root / "models" / "default";
        const std::string engine_sha256 = installed_engine_sha256(engine);
        const json checkpoint = installed_checkpoint_identity(model_root);
        config["engine"]["executable"] = engine.string();
        config["engine"]["working_directory"] = engine_root.string();
        config["engine"]["uci_options"]["ModelRoot"] = model_root.string();
        const auto timing_path = saved_locator_path(data_root, "timing_profile");
        const auto single_path =
            saved_locator_path(data_root, "lichess_single_game_profile");
        const auto capacity_path =
            saved_locator_path(data_root, "lichess_capacity_profile");
        bool exact_profile_set = false;
        if (timing_path.has_value() && single_path.has_value() &&
            capacity_path.has_value()) {
            try {
                require_exact_lichess_profiles(
                    *timing_path, *single_path, *capacity_path, engine_sha256, checkpoint,
                    config.at("capacity").at("total_slots").get<std::uint64_t>());
                exact_profile_set = true;
            } catch (const std::exception&) {
                // Locator imports are individually transactional. Keep the adapter safely
                // disabled while an operator replaces a multi-file qualification set.
            }
        }
        if (exact_profile_set) {
            config["engine"]["time_profile"] = timing_path->string();
            config["engine"]["single_game_profile"] = single_path->string();
            config["capacity"]["capacity_profile"] = capacity_path->string();
        } else {
            config["engine"].erase("time_profile");
            config["engine"].erase("single_game_profile");
            config["capacity"].erase("capacity_profile");
            config["scheduler"]["enabled"] = false;
            config["capacity"]["bot"]["target_active"] = 0;
        }
        if (const auto profile_path =
                saved_locator_path(data_root, "lichess_transport_profile");
            profile_path.has_value()) {
            config["timing"]["transport_profile_path"] = profile_path->string();
        }
        const json locator_options = installed_engine_locator_options(data_root);
        for (const auto& [name, value] : locator_options.items()) {
            config["engine"]["uci_options"][name] = value;
        }
        return config;
    }
    return json();
}

std::optional<json> generated_evidence_active_model_pointer(const json& config) {
    if (config.value("schema", "") != "uci_arena_evidence_service_config_v1" ||
        !config.contains("model_id")) {
        return std::nullopt;
    }
    return json{
        {"schema", "arena_installed_active_model_runtime_v1"},
        {"model_id", config.at("model_id")},
        {"model_root", config.at("model_root")},
        {"model_fingerprint", config.at("model_fingerprint")},
        {"provider", config.at("provider")},
        {"hardware_driver_fingerprint", config.value("hardware_driver_fingerprint", "")},
        {"engine_path", config.at("engine_path")},
        {"engine_sha256", config.at("engine_sha256")},
        {"uci_options", config.at("uci_options")},
    };
}

struct ImportWrite {
    std::string component_id;
    fs::path path;
    json document;
};

json invoke_book_forge_retry_auth(const SetupPaths& paths, const json& receipt) {
    if (receipt.value("component_id", "") != "book_forge.service") {
        throw std::runtime_error("Book Forge credential recovery received the wrong receipt");
    }
    const fs::path active_root =
        normalized_absolute(receipt.at("active_root").get<std::string>());
    static_cast<void>(validate_component_payload(
        active_root, "book_forge.service",
        receipt.at("component_version").get<std::string>()));
    const fs::path receipts_root = paths.data_root / "receipts";
    const fs::path node = installed_entrypoint(receipts_root, "node_runtime.private", "node");
    const fs::path cli = installed_entrypoint(receipts_root, "book_forge.service", "cli");
    const auto data_paths = receipt.value("data_paths", json::array());
    const std::string configuration_path = receipt.value("configuration_path", "");
    if (data_paths.empty() || !data_paths.at(0).is_string() ||
        configuration_path.empty()) {
        throw std::runtime_error("Book Forge credential recovery has no managed workspace");
    }
    const fs::path workspace =
        normalized_absolute(data_paths.at(0).get<std::string>());
    const fs::path configuration = normalized_absolute(configuration_path);
    if (!within(paths.workspace_root, workspace) ||
        !within(workspace, configuration) || !fs::is_regular_file(configuration) ||
        contains_symbolic_link(configuration)) {
        throw std::runtime_error(
            "Book Forge credential recovery configuration is unavailable");
    }
    arena::application::ProcessSupervisor process;
    const json response = process.run_json(
        node, active_root,
        {cli.string(), "acquisition", "retry-auth", "--workspace", workspace.string(),
         "--config", configuration.string(), "--json"},
        std::chrono::seconds(30));
    const json acquisition = response.value("acquisition", json::object());
    if (!response.value("ok", false) || !response.contains("retried") ||
        !response.at("retried").is_number_unsigned() ||
        !acquisition.value("credentialConfigured", false) ||
        acquisition.value("authState", "") != "ready") {
        throw std::runtime_error(
            "Book Forge did not confirm ready credential configuration");
    }
    return {
        {"retried", response.at("retried")},
        {"credential_configured", true},
        {"auth_state", "ready"},
    };
}

json refresh_import_consumers(const SetupPaths& paths, const std::string& kind) {
    std::vector<std::string> affected;
    if (kind == "timing_profile") {
        affected = {"uci_arena_evidence.service", "lichess.adapter"};
    } else if (kind == "opening_book" || kind == "syzygy" ||
               kind == "lichess_single_game_profile" ||
               kind == "lichess_capacity_profile" ||
               kind == "lichess_transport_profile") {
        affected = {"lichess.adapter"};
    } else if (kind == "lichess_token_path") {
        affected = {"book_forge.service"};
    } else {
        return json::array();
    }

    const fs::path receipts_root = paths.data_root / "receipts";
    std::vector<ImportWrite> writes;
    std::set<std::string> refreshed_components;
    for (const auto& component_id : affected) {
        const fs::path receipt_path = receipts_root / (component_id + ".json");
        if (!fs::is_regular_file(receipt_path)) {
            continue;
        }
        const json receipt = read_json(receipt_path);
        if (receipt.value("schema", "") != "arena_install_receipt_v1" ||
            receipt.value("component_id", "") != component_id ||
            receipt.value("state", "") != "installed_managed") {
            continue;
        }
        const fs::path component_root =
            normalized_absolute(receipt.at("active_root").get<std::string>());
        static_cast<void>(validate_component_payload(
            component_root, component_id, receipt.at("component_version").get<std::string>()));
        const std::string configured = receipt.value("configuration_path", "");
        if (configured.empty()) {
            throw std::runtime_error("installed consumer has no generated configuration: " +
                                     component_id);
        }
        const fs::path configuration_path = normalized_absolute(configured);
        const fs::path workspace = configuration_path.parent_path();
        if (!within(paths.workspace_root, configuration_path) ||
            !within(paths.workspace_root, workspace)) {
            throw std::runtime_error(
                "installed consumer configuration escapes the selected workspace");
        }
        const json generated =
            generated_component_config(component_id, receipts_root, workspace, component_root);
        if (const auto pointer = generated_evidence_active_model_pointer(generated);
            pointer.has_value()) {
            const fs::path pointer_path =
                normalized_absolute(generated.at("activeModelPointer").get<std::string>());
            if (!within(workspace, pointer_path)) {
                throw std::runtime_error(
                    "generated Evidence active-model pointer escapes its workspace");
            }
            writes.push_back({component_id, pointer_path, *pointer});
        }
        writes.push_back({component_id, configuration_path, generated});
        refreshed_components.insert(component_id);
    }

    struct AppliedWrite {
        fs::path path;
        fs::path backup;
        bool existed;
    };
    std::vector<AppliedWrite> applied;
    const std::string transaction_id =
        std::to_string(std::chrono::system_clock::now().time_since_epoch().count());
    try {
        for (const auto& write : writes) {
            fs::create_directories(write.path.parent_path());
            const bool existed = fs::is_regular_file(write.path);
            const fs::path backup = write.path.string() + ".import-rollback-" + transaction_id;
            std::error_code ignored;
            fs::remove(backup, ignored);
            if (existed) {
                fs::copy_file(write.path, backup, fs::copy_options::overwrite_existing);
            }
            applied.push_back({write.path, backup, existed});
            write_json_atomic(write.path, write.document);
            if (read_json(write.path) != write.document) {
                throw std::runtime_error(
                    "generated import consumer configuration verification failed");
            }
        }
        if (kind == "lichess_token_path" &&
            refreshed_components.contains("book_forge.service") &&
            fs::is_regular_file(receipts_root / "node_runtime.private.json")) {
            static_cast<void>(invoke_book_forge_retry_auth(
                paths, read_json(receipts_root / "book_forge.service.json")));
        }
    } catch (...) {
        std::error_code ignored;
        for (auto current = applied.rbegin(); current != applied.rend(); ++current) {
            fs::remove(current->path, ignored);
            ignored.clear();
            if (current->existed && fs::is_regular_file(current->backup)) {
                fs::rename(current->backup, current->path, ignored);
                ignored.clear();
            } else {
                fs::remove(current->backup, ignored);
                ignored.clear();
            }
        }
        throw;
    }

    std::error_code ignored;
    for (const auto& write : applied) {
        fs::remove(write.backup, ignored);
        ignored.clear();
    }
    json refreshed = json::array();
    for (const auto& component_id : refreshed_components) {
        refreshed.push_back(component_id);
    }
    return refreshed;
}

json save_locator_and_refresh(const SetupPaths& paths, const std::string& kind,
                              const fs::path& normalized, const json& validation,
                              const std::string_view mode = "in_place_reference") {
    if (mode != "in_place_reference" &&
        !(kind == "lichess_single_game_profile" && mode == "managed_copy")) {
        throw std::runtime_error("saved locator mode is not allowed for " + kind);
    }
    const fs::path locator_path = paths.data_root / "locators" / (kind + ".json");
    fs::create_directories(locator_path.parent_path());
    const bool locator_existed = fs::is_regular_file(locator_path);
    const fs::path backup = locator_path.string() + ".import-rollback";
    std::error_code ignored;
    fs::remove(backup, ignored);
    if (locator_existed) {
        fs::copy_file(locator_path, backup, fs::copy_options::overwrite_existing);
    }
    const json locator{
        {"schema", "arena_saved_locator_v1"},
        {"schema_version", 1},
        {"kind", kind},
        {"mode", mode},
        {"path", normalized.string()},
        {"validation", validation},
        {"saved_at", current_utc_iso()},
    };
    try {
        write_json_atomic(locator_path, locator);
        const json refreshed = refresh_import_consumers(paths, kind);
        if (read_json(locator_path) != locator) {
            throw std::runtime_error("saved locator verification failed");
        }
        fs::remove(backup, ignored);
        return refreshed;
    } catch (...) {
        fs::remove(locator_path, ignored);
        ignored.clear();
        if (locator_existed && fs::is_regular_file(backup)) {
            fs::rename(backup, locator_path, ignored);
        } else {
            fs::remove(backup, ignored);
        }
        throw;
    }
}

bool supports_startup_capability(const json& receipt, const std::string_view capability) {
    if (!receipt.contains("active_root") || !receipt.at("active_root").is_string()) {
        return false;
    }
    const fs::path manifest_path =
        fs::path(receipt.at("active_root").get<std::string>()) / "arena-component.json";
    if (!fs::is_regular_file(manifest_path) || fs::is_symlink(manifest_path)) {
        return false;
    }
    const json manifest = read_json(manifest_path);
    const auto capabilities = manifest.value("capabilities", json::array());
    return capabilities.is_array() && std::find(capabilities.begin(), capabilities.end(),
                                                std::string(capability)) != capabilities.end();
}

json invoke_provider_startup(const SetupPaths& paths, const json& receipt,
                             const std::string& action) {
    const std::string component_id = receipt.at("component_id").get<std::string>();
    const fs::path active_root = receipt.at("active_root").get<std::string>();
    const json manifest = validate_component_payload(
        active_root, component_id, receipt.at("component_version").get<std::string>());
    const fs::path receipts_root = paths.data_root / "receipts";
    const fs::path node = installed_entrypoint(receipts_root, "node_runtime.private", "node");
    const json entrypoints = manifest.at("entrypoints");
    std::string control_entrypoint;
    if (entrypoints.contains("startup")) {
        control_entrypoint = "startup";
    } else if (entrypoints.contains("cli")) {
        control_entrypoint = "cli";
    } else if (entrypoints.contains("adapter")) {
        control_entrypoint = "adapter";
    } else {
        throw std::runtime_error("startup-capable component has no declared control entrypoint: " +
                                 component_id);
    }
    const fs::path cli = installed_entrypoint(receipts_root, component_id, control_entrypoint);
    const auto data_paths = receipt.value("data_paths", json::array());
    if (data_paths.empty() || !data_paths.at(0).is_string()) {
        throw std::runtime_error("startup-capable component has no managed workspace: " +
                                 component_id);
    }
    const fs::path workspace = normalized_absolute(data_paths.at(0).get<std::string>());
    if (!within(paths.workspace_root, workspace)) {
        throw std::runtime_error(
            "startup-capable component workspace is outside the selected workspace "
            "root");
    }
    std::vector<std::string> arguments{
        cli.string(),
        "startup",
        action,
    };
    if (control_entrypoint == "adapter") {
        arguments.push_back("--runtime-dir");
        arguments.push_back((workspace / "runtime").string());
    } else {
        arguments.push_back("--workspace");
        arguments.push_back(workspace.string());
    }
    const std::string configuration_path = receipt.value("configuration_path", "");
    if (!configuration_path.empty()) {
        const fs::path configuration = normalized_absolute(configuration_path);
        if (!within(workspace, configuration) || !fs::is_regular_file(configuration) ||
            fs::is_symlink(configuration)) {
            throw std::runtime_error("startup-capable component configuration is unavailable");
        }
        arguments.push_back("--config");
        arguments.push_back(configuration.string());
    }
    arena::application::ProcessSupervisor process;
    const json response = process.run_json(node, active_root, arguments, std::chrono::seconds(30));
    const json status = response.contains("startup") ? response.at("startup") : response;
    if (!status.is_object() || !status.contains("installed") ||
        !status.at("installed").is_boolean()) {
        throw std::runtime_error("provider startup adapter returned an invalid status: " +
                                 component_id);
    }
    const bool expected_installed = action != "uninstall";
    if (status.at("installed").get<bool>() != expected_installed) {
        throw std::runtime_error("provider startup adapter did not confirm " + action + ": " +
                                 component_id);
    }
    if (expected_installed && status.contains("matchesCurrentConfiguration") &&
        (!status.at("matchesCurrentConfiguration").is_boolean() ||
         !status.at("matchesCurrentConfiguration").get<bool>())) {
        throw std::runtime_error("provider startup adapter configuration does not match: " +
                                 component_id);
    }
    return status;
}

void remove_managed_component_tree(const fs::path& active_root, const std::string& component_id) {
    constexpr int attempts = 100;
    constexpr auto retry_delay = std::chrono::milliseconds(100);
    std::string failure = "path remains present";
    for (int attempt = 0; attempt < attempts; ++attempt) {
        std::error_code remove_error;
        fs::remove_all(active_root, remove_error);
        std::error_code exists_error;
        const bool remains = fs::exists(active_root, exists_error);
        if (!remove_error && !exists_error && !remains) {
            return;
        }
        if (remove_error) {
            failure = remove_error.message();
        } else if (exists_error) {
            failure = exists_error.message();
        }
        if (attempt + 1 < attempts) {
            std::this_thread::sleep_for(retry_delay);
        }
    }
    throw std::runtime_error("cannot remove managed component " + component_id +
                             " after shutdown: " + failure);
}

} // namespace

json copy_managed_lichess_profile(const SetupPaths& paths, const fs::path& source,
                                  const json& validation);

std::string SetupEngine::platform_key() {
#if defined(_WIN32)
    return sizeof(void*) == 8 ? "windows-x86_64" : "windows-x86";
#elif defined(__aarch64__)
    return "linux-aarch64";
#else
    return "linux-x86_64";
#endif
}

json SetupEngine::load_catalog(const fs::path& catalog_path, const fs::path& metadata_directory,
                               const bool allow_unsigned_development_catalog,
                               const fs::path& rollback_state_path) {
    if (metadata_directory.empty()) {
        if (!allow_unsigned_development_catalog) {
            throw std::runtime_error("trusted TUF metadata is required; unsigned catalogs are "
                                     "development-only");
        }
    } else {
        verify_tuf_chain(catalog_path, metadata_directory, rollback_state_path);
    }
    json catalog = read_json(catalog_path);
    if (catalog.value("schema", "") != "arena_install_catalog_v1" ||
        catalog.value("schema_version", 0) != 1 || !catalog.contains("components") ||
        !catalog.at("components").is_array() || !catalog.contains("presets") ||
        !catalog.at("presets").is_object()) {
        throw std::runtime_error("installation catalog does not satisfy arena_install_catalog_v1");
    }
    std::set<std::string> ids;
    for (const auto& component : catalog.at("components")) {
        const std::string id = component.value("id", "");
        if (!valid_identifier(id) || !ids.insert(id).second || !component.contains("version") ||
            !component.contains("artifacts")) {
            throw std::runtime_error("installation catalog has an invalid component identity");
        }
    }
    catalog["_catalog_directory"] = normalized_absolute(catalog_path.parent_path()).string();
    return catalog;
}

json SetupEngine::persist_release_media(const json& catalog, const SetupPaths& paths,
                                        const fs::path& catalog_path,
                                        const fs::path& metadata_directory) {
    const fs::path source_catalog = normalized_absolute(catalog_path);
    const fs::path source_root = source_catalog.parent_path();
    if (!fs::is_regular_file(source_catalog) || contains_symbolic_link(source_catalog)) {
        throw std::runtime_error("verified release catalog is unavailable for retention");
    }
    const fs::path release_root = paths.data_root / "release";
    const std::string transaction_id =
        std::to_string(std::chrono::system_clock::now().time_since_epoch().count());
    const fs::path stage = paths.data_root / ("release.staging-" + transaction_id);
    const fs::path backup = paths.data_root / ("release.rollback-" + transaction_id);
    std::error_code ignored;
    fs::create_directories(paths.data_root);
    fs::remove_all(stage, ignored);
    ignored.clear();
    fs::remove_all(backup, ignored);
    try {
        fs::create_directories(stage);
        fs::copy_file(source_catalog, stage / "catalog.json", fs::copy_options::overwrite_existing);

        if (!metadata_directory.empty()) {
            const fs::path metadata_root = normalized_absolute(metadata_directory);
            for (const auto* required :
                 {"root.json", "targets.json", "snapshot.json", "timestamp.json"}) {
                const fs::path source = metadata_root / required;
                if (!fs::is_regular_file(source) || contains_symbolic_link(source)) {
                    throw std::runtime_error(std::string("verified TUF metadata is unavailable: ") +
                                             required);
                }
            }
            fs::create_directories(stage / "tuf");
            for (const auto& entry : fs::directory_iterator(metadata_root)) {
                if (entry.is_symlink() || !entry.is_regular_file() ||
                    entry.path().extension() != ".json" ||
                    entry.file_size() > 8ULL * 1024ULL * 1024ULL) {
                    continue;
                }
                fs::copy_file(entry.path(), stage / "tuf" / entry.path().filename(),
                              fs::copy_options::overwrite_existing);
            }
        }

        json retained = json::array();
        std::set<std::string> retained_sources;
        for (const auto& component : catalog.at("components")) {
            const json& artifact = platform_artifact(component);
            const std::string source = artifact.at("source").get<std::string>();
            if (source.starts_with("https://")) {
                continue;
            }
            const fs::path relative = fs::path(source).lexically_normal();
            if (!safe_relative_path(relative) ||
                !retained_sources.insert(relative.generic_string()).second) {
                if (retained_sources.contains(relative.generic_string())) {
                    continue;
                }
                throw std::runtime_error("release catalog contains an unsafe retained source");
            }
            const fs::path source_file = source_root / relative;
            if (!within(source_root, source_file) || !fs::is_regular_file(source_file) ||
                contains_symbolic_link(source_file) ||
                fs::file_size(source_file) != integer_field(artifact, "size_bytes") ||
                sha256_file(source_file) != artifact.at("sha256").get<std::string>()) {
                throw std::runtime_error(
                    "release payload cannot be retained with exact identity: " +
                    relative.generic_string());
            }
            const fs::path destination = stage / relative;
            if (!within(stage, destination)) {
                throw std::runtime_error("retained release payload escapes staging");
            }
            fs::create_directories(destination.parent_path());
            const fs::path cached = paths.cache_root / artifact.at("sha256").get<std::string>();
            const fs::path link_source =
                fs::is_regular_file(cached) &&
                        sha256_file(cached) == artifact.at("sha256").get<std::string>()
                    ? cached
                    : source_file;
            std::error_code link_error;
            fs::create_hard_link(link_source, destination, link_error);
            if (link_error) {
                fs::copy_file(source_file, destination, fs::copy_options::overwrite_existing);
            }
            retained.push_back(relative.generic_string());
        }

        const bool had_existing = fs::exists(release_root);
        if (had_existing) {
            fs::rename(release_root, backup);
        }
        try {
            fs::rename(stage, release_root);
        } catch (...) {
            if (had_existing && fs::exists(backup) && !fs::exists(release_root)) {
                fs::rename(backup, release_root);
            }
            throw;
        }
        fs::remove_all(backup, ignored);
        return {
            {"schema", "arena_retained_release_media_v1"},
            {"catalog", (release_root / "catalog.json").string()},
            {"metadata",
             metadata_directory.empty() ? json(nullptr) : json((release_root / "tuf").string())},
            {"payloads", std::move(retained)},
        };
    } catch (...) {
        fs::remove_all(stage, ignored);
        throw;
    }
}

json SetupEngine::inventory(const SetupPaths& paths, const std::vector<fs::path>& extra_roots) {
    json knowledge{
        {"opening_book",
         {{"state", "missing"},
          {"path", paths.knowledge_root.string()},
          {"missing_files", json::array()}}},
        {"syzygy",
         {{"state", "missing"},
          {"path", paths.knowledge_root.string()},
          {"wdl_files", 0},
          {"dtz_files", 0},
          {"bytes", 0}}},
    };
    const fs::path book_root = paths.knowledge_root / "opening_books";
    const std::array<const char*, 4> book_files{
        "strong_rare_v1.bin",
        "strong_rare_v1.ffstats",
        "strong_rare_v1.policy",
        "snapshot.manifest.json",
    };
    for (const char* name : book_files) {
        if (!fs::is_regular_file(book_root / name) || contains_symbolic_link(book_root / name)) {
            knowledge["opening_book"]["missing_files"].push_back(name);
        }
    }
    if (knowledge["opening_book"]["missing_files"].empty()) {
        knowledge["opening_book"]["state"] = "detected_unmanaged";
        knowledge["opening_book"]["path"] = book_root.string();
        const json managed = managed_knowledge_components(paths, "opening_book.");
        if (!managed.empty()) {
            knowledge["opening_book"]["state"] = "installed_managed";
            knowledge["opening_book"]["managed_components"] = managed;
        }
    }
    const fs::path tablebase_root = paths.knowledge_root / "tablebases";
    if (fs::is_directory(tablebase_root) && !contains_symbolic_link(tablebase_root)) {
        for (const auto& entry : fs::directory_iterator(tablebase_root)) {
            if (!entry.is_regular_file() || entry.is_symlink()) {
                continue;
            }
            if (entry.path().extension() == ".rtbw") {
                knowledge["syzygy"]["wdl_files"] =
                    knowledge["syzygy"]["wdl_files"].get<std::uint64_t>() + 1;
            } else if (entry.path().extension() == ".rtbz") {
                knowledge["syzygy"]["dtz_files"] =
                    knowledge["syzygy"]["dtz_files"].get<std::uint64_t>() + 1;
            } else {
                continue;
            }
            knowledge["syzygy"]["bytes"] =
                knowledge["syzygy"]["bytes"].get<std::uint64_t>() + entry.file_size();
        }
        if (knowledge["syzygy"]["wdl_files"].get<std::uint64_t>() > 0) {
            knowledge["syzygy"]["state"] = "detected_unmanaged";
            knowledge["syzygy"]["path"] = tablebase_root.string();
            const json managed = managed_knowledge_components(paths, "syzygy.");
            if (!managed.empty()) {
                knowledge["syzygy"]["state"] = "installed_managed";
                knowledge["syzygy"]["managed_components"] = managed;
            }
        }
    }
    json discovery = discovery_rows(paths, extra_roots);
    if (knowledge["opening_book"]["state"] == "missing") {
        const json& selection = discovery.at("opening_book").at("selection");
        if (selection.value("state", "") != "unresolved" &&
            selection.value("state", "") != "broken" && selection.contains("path") &&
            selection.at("path").is_string()) {
            try {
                const fs::path selected = selection.at("path").get<std::string>();
                knowledge["opening_book"]["validation"] = validate_book_locator(selected);
                knowledge["opening_book"]["state"] = "detected_unmanaged";
                knowledge["opening_book"]["path"] = normalized_absolute(selected).string();
                knowledge["opening_book"]["source"] = selection.value("source", "discovered");
            } catch (const std::exception&) {
            }
        }
    }
    if (knowledge["syzygy"]["state"] == "missing") {
        const json& selection = discovery.at("syzygy").at("selection");
        if (selection.value("state", "") != "unresolved" &&
            selection.value("state", "") != "broken" && selection.contains("path") &&
            selection.at("path").is_string()) {
            try {
                const fs::path selected = selection.at("path").get<std::string>();
                const json validation = validate_syzygy_locator(selected);
                knowledge["syzygy"]["state"] = "detected_unmanaged";
                knowledge["syzygy"]["path"] = normalized_absolute(selected).string();
                knowledge["syzygy"]["source"] = selection.value("source", "discovered");
                knowledge["syzygy"]["wdl_files"] = validation.at("wdl_files");
                knowledge["syzygy"]["dtz_files"] = validation.at("dtz_files");
                knowledge["syzygy"]["bytes"] = validation.at("bytes");
            } catch (const std::exception&) {
            }
        }
    }

    json candidates = json::array();
    std::vector<fs::path> bounded_roots = extra_roots;
    if (const char* path_environment = std::getenv("PATH")) {
#ifdef _WIN32
        constexpr char separator = ';';
#else
        constexpr char separator = ':';
#endif
        std::stringstream paths_stream(path_environment);
        for (std::string item; std::getline(paths_stream, item, separator);) {
            if (!item.empty()) {
                bounded_roots.emplace_back(item);
            }
        }
    }
    const std::set<std::string> candidate_names{
#ifdef _WIN32
        "uci_arena_engine.exe", "stockfish.exe", "lc0.exe"
#else
        "uci_arena_engine", "stockfish", "lc0"
#endif
    };
    std::set<std::string> seen;
    for (const auto& root : bounded_roots) {
        if (!fs::is_directory(root) || contains_symbolic_link(root)) {
            continue;
        }
        for (const auto& name : candidate_names) {
            const fs::path candidate = root / name;
            if (fs::is_regular_file(candidate) && !contains_symbolic_link(candidate)) {
                const std::string normalized = normalized_absolute(candidate).string();
                if (seen.insert(normalized).second) {
                    candidates.push_back({{"kind", "uci_engine"},
                                          {"state", "detected_unmanaged"},
                                          {"path", normalized},
                                          {"requires_probe", true}});
                }
            }
        }
    }

    json restaurant{{"state", "absent"}};
    if (const char* configured = std::getenv("RESTAURANT_ROOT");
        configured != nullptr && fs::is_directory(configured) &&
        !contains_symbolic_link(configured)) {
        restaurant = {
            {"state", "detected_unmanaged"},
            {"path", normalized_absolute(configured).string()},
            {"installable", false},
        };
    }
    return {
        {"schema", "arena_install_inventory_v1"},
        {"platform", platform_key()},
        {"managed_components", receipt_rows(paths)},
        {"knowledge", std::move(knowledge)},
        {"candidates", std::move(candidates)},
        {"discovery", std::move(discovery)},
        {"restaurant", std::move(restaurant)},
    };
}

json SetupEngine::import_path(const SetupPaths& paths, const std::string& kind,
                              const fs::path& path, const bool managed_copy) {
    const fs::path normalized = normalized_absolute(path);
    const json validation = validate_import_locator(kind, normalized);
    if (kind == "lichess_single_game_profile") {
        json result = copy_managed_lichess_profile(paths, normalized, validation);
        const fs::path active_profile =
            normalized_absolute(result.at("path").get<std::string>());
        const fs::path active_root =
            normalized_absolute(result.at("receipt").at("active_root").get<std::string>());
        const fs::path receipt_path =
            paths.data_root / "receipts" /
            (result.at("receipt").at("component_id").get<std::string>() + ".json");
        const bool active_root_existed = result.at("_active_root_existed").get<bool>();
        const bool receipt_existed = result.at("_receipt_existed").get<bool>();
        try {
            const json managed_validation =
                validate_import_locator(kind, active_profile);
            result["validation"] = managed_validation;
            result["refreshed_consumers"] = save_locator_and_refresh(
                paths, kind, active_profile, managed_validation, "managed_copy");
        } catch (...) {
            std::error_code ignored;
            if (!receipt_existed) {
                fs::remove(receipt_path, ignored);
                ignored.clear();
            }
            if (!active_root_existed) {
                fs::remove_all(active_root, ignored);
            }
            throw;
        }
        result.erase("_active_root_existed");
        result.erase("_receipt_existed");
        return result;
    }
    if (managed_copy) {
        if (kind != "opening_book") {
            throw std::runtime_error("managed copy is allowed only for opening books");
        }
        json result = copy_managed_book(paths, normalized, validation);
        const fs::path active_root = normalized_absolute(result.at("path").get<std::string>());
        const fs::path receipt_path =
            paths.data_root / "receipts" /
            (result.at("receipt").at("component_id").get<std::string>() + ".json");
        const bool active_root_existed = result.at("_active_root_existed").get<bool>();
        const bool receipt_existed = result.at("_receipt_existed").get<bool>();
        try {
            const json managed_validation = validate_import_locator(kind, active_root);
            result["refreshed_consumers"] =
                save_locator_and_refresh(paths, kind, active_root, managed_validation);
        } catch (...) {
            std::error_code ignored;
            if (!receipt_existed) {
                fs::remove(receipt_path, ignored);
                ignored.clear();
            }
            if (!active_root_existed) {
                fs::remove_all(active_root, ignored);
            }
            throw;
        }
        result.erase("_active_root_existed");
        result.erase("_receipt_existed");
        return result;
    }

    return {
        {"schema", "arena_setup_import_result_v1"},
        {"schema_version", 1},
        {"kind", kind},
        {"mode", "in_place_reference"},
        {"state", "saved"},
        {"path", normalized.string()},
        {"validation", validation},
        {"refreshed_consumers", save_locator_and_refresh(paths, kind, normalized, validation)},
    };
}

json SetupEngine::plan(const json& catalog, const SetupPaths& paths, const std::string& preset,
                       const std::vector<std::string>& selected_components, const bool offline,
                       const bool download_missing_knowledge, const bool download_missing_books,
                       const bool download_missing_tablebases, const bool repair) {
    const auto ids = selected_ids(catalog, preset, selected_components, download_missing_knowledge,
                                  download_missing_books, download_missing_tablebases);
    json actions = json::array();
    std::uint64_t download = 0;
    std::uint64_t staging = 0;
    std::uint64_t installed = 0;
    std::uint64_t knowledge_activation = 0;
    std::uint64_t backup = 0;
    std::uint64_t reserve = 0;
    std::set<std::string> artifact_hashes;
    for (const auto& id : ids) {
        const auto& component = component_by_id(catalog, id);
        reserve = (std::max)(reserve, integer_field(component, "workspace_reserve_bytes"));
        const fs::path receipt_path = paths.data_root / "receipts" / (id + ".json");
        bool already_current = false;
        bool current_enabled = true;
        if (fs::is_regular_file(receipt_path)) {
            try {
                const json receipt = read_json(receipt_path);
                if (receipt.value("component_id", "") == id &&
                    receipt.value("component_version", "") ==
                        component.at("version").get<std::string>() &&
                    (receipt.value("state", "") == "installed_managed" ||
                     receipt.value("state", "") == "installed_disabled")) {
                    validate_component_payload(receipt.at("active_root").get<std::string>(), id,
                                               receipt.at("component_version").get<std::string>());
                    already_current = true;
                    current_enabled = receipt.value("state", "") == "installed_managed";
                    if (!repair) {
                        actions.push_back({{"action", "keep"},
                                           {"component_id", id},
                                           {"version", component.at("version")},
                                           {"enabled", current_enabled}});
                        continue;
                    }
                }
            } catch (const std::exception&) {
            }
        }
        const auto& artifact = platform_artifact(component);
        const std::string hash = artifact.at("sha256").get<std::string>();
        if (artifact_hashes.insert(hash).second) {
            download += artifact.value("type", "archive") == "file_set"
                            ? integer_field(artifact, "size_bytes") +
                                  integer_field(artifact, "total_size_bytes")
                            : integer_field(artifact, "size_bytes");
            staging += integer_field(component, "staging_bytes");
        }
        installed += integer_field(component, "installed_bytes");
        if (id == "opening_book.standard" || id.rfind("syzygy.", 0) == 0) {
            knowledge_activation += integer_field(component, "installed_bytes");
        }
        backup += integer_field(component, "backup_bytes");
        actions.push_back({{"action", already_current ? "repair" : "install"},
                           {"component_id", id},
                           {"version", component.at("version")},
                           {"dependencies", component.value("dependencies", json::array())},
                           {"enabled", already_current ? current_enabled : true},
                           {"artifact", artifact}});
    }
    json blockers = json::array();
    if (std::find(ids.begin(), ids.end(), "lichess.adapter") != ids.end()) {
        for (const auto* kind :
             {"timing_profile", "lichess_single_game_profile",
              "lichess_capacity_profile", "lichess_transport_profile"}) {
            try {
                if (!saved_locator_path(paths.data_root, kind).has_value()) {
                    blockers.push_back(
                        {{"code", "required_locator_missing"},
                         {"component_id", "lichess.adapter"},
                         {"kind", kind}});
                }
            } catch (const std::exception& error) {
                blockers.push_back(
                    {{"code", "required_locator_invalid"},
                     {"component_id", "lichess.adapter"},
                     {"kind", kind},
                     {"detail", error.what()}});
            }
        }
    }
    std::uint64_t effective_reserve = reserve;
    if (preset != "complete_arena" && reserve > 0) {
        const fs::space_info workspace_space =
            fs::space(nearest_existing_ancestor(paths.workspace_root));
        if (workspace_space.available < reserve) {
            for (auto& action : actions) {
                const std::string id = action.at("component_id").get<std::string>();
                if ((action.value("action", "") == "install" ||
                     action.value("action", "") == "repair") &&
                    (id == "book_forge.service" || id == "uci_arena_evidence.service")) {
                    action["enabled"] = false;
                    action["warning"] = "installed disabled until workspace reserve is available";
                }
            }
            effective_reserve = 0;
        }
    }
    const std::uint64_t headroom = (std::max)(gib, staging / 10);
    std::map<std::string, json> volumes;
    const auto allocate = [&volumes](const fs::path& path, const std::string& role,
                                     const char* field, const std::uint64_t bytes) {
        if (bytes == 0) {
            return;
        }
        const fs::path capacity_path = nearest_existing_ancestor(path);
        const std::string key = volume_key(capacity_path);
        if (!volumes.contains(key)) {
            const auto space = fs::space(capacity_path);
            volumes[key] = {
                {"volume_id", key},
                {"capacity_path", capacity_path.string()},
                {"roles", json::array()},
                {"download_cache_bytes", 0},
                {"staging_bytes", 0},
                {"installed_bytes", 0},
                {"backup_bytes", 0},
                {"operational_reserve_bytes", 0},
                {"headroom_bytes", 0},
                {"peak_required_bytes", 0},
                {"available_bytes", space.available},
            };
        }
        auto& volume = volumes.at(key);
        if (std::find(volume["roles"].begin(), volume["roles"].end(), role) ==
            volume["roles"].end()) {
            volume["roles"].push_back(role);
        }
        volume[field] = volume.at(field).get<std::uint64_t>() + bytes;
        volume["peak_required_bytes"] =
            volume.at("peak_required_bytes").get<std::uint64_t>() + bytes;
    };
    allocate(paths.cache_root, "download_cache", "download_cache_bytes", download);
    allocate(paths.install_root, "staging", "staging_bytes", staging);
    allocate(paths.install_root, "components", "installed_bytes", installed);
    allocate(paths.install_root, "rollback", "backup_bytes", backup);
    allocate(paths.install_root, "staging_headroom", "headroom_bytes", headroom);
    allocate(paths.knowledge_root, "knowledge_activation", "installed_bytes", knowledge_activation);
    allocate(paths.workspace_root, "operational_workspace", "operational_reserve_bytes",
             effective_reserve);

    json volume_rows = json::array();
    std::uint64_t peak = 0;
    for (auto& [key, volume] : volumes) {
        (void)key;
        const auto required = volume.at("peak_required_bytes").get<std::uint64_t>();
        const auto available = volume.at("available_bytes").get<std::uint64_t>();
        peak += required;
        if (available < required) {
            const bool workspace_volume =
                std::find(volume["roles"].begin(), volume["roles"].end(),
                          "operational_workspace") != volume["roles"].end();
            blockers.push_back(
                {{"code", preset == "complete_arena" && workspace_volume ? "workspace_reserve_unmet"
                                                                         : "insufficient_space"},
                 {"volume_id", volume.at("volume_id")},
                 {"path", volume.at("capacity_path")},
                 {"available_bytes", available},
                 {"required_bytes", required}});
        }
        volume_rows.push_back(volume);
    }
    return {
        {"schema", "arena_install_plan_v1"},
        {"schema_version", 1},
        {"suite_version", catalog.at("suite_version")},
        {"platform", platform_key()},
        {"preset", preset},
        {"offline", offline},
        {"repair", repair},
        {"download_missing_knowledge", download_missing_knowledge},
        {"download_missing_books", download_missing_knowledge || download_missing_books},
        {"download_missing_tablebases", download_missing_knowledge || download_missing_tablebases},
        {"catalog_directory", catalog.value("_catalog_directory", ".")},
        {"paths",
         {{"install_root", paths.install_root.string()},
          {"data_root", paths.data_root.string()},
          {"cache_root", paths.cache_root.string()},
          {"workspace_root", paths.workspace_root.string()},
          {"knowledge_root", paths.knowledge_root.string()}}},
        {"space",
         {{"download_cache_bytes", download},
          {"staging_bytes", staging},
          {"installed_bytes", installed},
          {"knowledge_activation_bytes", knowledge_activation},
          {"backup_bytes", backup},
          {"operational_reserve_bytes", effective_reserve},
          {"headroom_bytes", headroom},
          {"peak_required_bytes", peak},
          {"volumes", std::move(volume_rows)}}},
        {"actions", std::move(actions)},
        {"blockers", std::move(blockers)},
    };
}

json SetupEngine::apply(const json&, const json& plan, const SetupPaths& paths,
                        const bool confirmed, const bool repair) {
    if (!confirmed) {
        throw std::runtime_error("apply requires explicit confirmation");
    }
    if (plan.value("schema", "") != "arena_install_plan_v1" || !plan.at("blockers").empty()) {
        throw std::runtime_error("installation plan is invalid or blocked");
    }
    if (!paths.bundle_owner.empty() && !valid_bundle_owner(paths.bundle_owner)) {
        throw std::runtime_error("bundle owner is invalid");
    }
    const fs::path components_root = paths.install_root / "components";
    const fs::path staging_root = paths.install_root / ".staging";
    const fs::path receipts_root = paths.data_root / "receipts";
    const fs::path transactions_root = paths.data_root / "transactions";
    fs::create_directories(components_root);
    fs::create_directories(staging_root);
    fs::create_directories(receipts_root);
    fs::create_directories(transactions_root);
    const std::string transaction_id =
        std::to_string(std::chrono::system_clock::now().time_since_epoch().count());
    const fs::path journal_path = transactions_root / (transaction_id + ".json");
    json journal{
        {"schema", "arena_install_transaction_v1"},
        {"transaction_id", transaction_id},
        {"state", "running"},
        {"actions", json::array()},
    };
    write_json_atomic(journal_path, journal);
    json results = json::array();
    try {
        const fs::path catalog_directory = fs::path(plan.value("catalog_directory", "."));
        for (const auto& action : plan.at("actions")) {
            if (action.value("action", "install") == "keep") {
                results.push_back({{"component_id", action.at("component_id")},
                                   {"component_version", action.at("version")},
                                   {"state", "already_current"}});
                continue;
            }
            const std::string id = action.at("component_id").get<std::string>();
            const std::string version = action.at("version").get<std::string>();
            if (!valid_identifier(id) || !valid_identifier(version)) {
                throw std::runtime_error("plan contains an unsafe component identity");
            }
            journal["actions"].push_back(
                {{"component_id", id}, {"version", version}, {"state", "staging"}});
            write_json_atomic(journal_path, journal);
            const fs::path artifact = acquire_artifact(action.at("artifact"), catalog_directory,
                                                       paths, plan.value("offline", false));
            const fs::path stage = staging_root / transaction_id / id;
            std::error_code ignored;
            fs::remove_all(stage, ignored);
            json manifest;
            if (action.at("artifact").value("type", "archive") == "file_set") {
                manifest = stage_file_set(artifact, action.at("artifact"), catalog_directory, paths,
                                          plan.value("offline", false), stage, id, version);
            } else {
                extract_archive(artifact, stage);
                manifest = validate_component_payload(stage, id, version);
            }
            manifest = validate_component_payload(stage, id, version);

            const fs::path component_root = components_root / id;
            const fs::path target = component_root / version;
            if (!within(components_root, target)) {
                throw std::runtime_error("component target escapes install root");
            }
            fs::create_directories(component_root);
            journal["actions"].back()["target"] = target.string();
            if (fs::exists(target)) {
                if (!repair) {
                    throw std::runtime_error(
                        "component version already exists; use repair or upgrade");
                }
                const fs::path replaced =
                    component_root / (version + ".rollback-" + transaction_id);
                fs::rename(target, replaced);
                journal["actions"].back()["rollback_path"] = replaced.string();
            }
            fs::rename(stage, target);
            journal["actions"].back()["state"] = "target_activated";
            write_json_atomic(journal_path, journal);

            const json knowledge_activation =
                activate_knowledge(target, manifest, paths, transaction_id, id);
            if (!knowledge_activation.empty()) {
                journal["actions"].back()["knowledge_activation"] = knowledge_activation;
                const std::string locator_kind =
                    id.rfind("opening_book.", 0) == 0
                        ? "opening_book"
                        : id.rfind("syzygy.", 0) == 0 ? "syzygy" : "";
                if (!locator_kind.empty()) {
                    const fs::path locator_path =
                        paths.data_root / "locators" / (locator_kind + ".json");
                    if (!fs::exists(locator_path)) {
                        const fs::path activation_path =
                            normalized_absolute(
                                knowledge_activation.at("path").get<std::string>());
                        const json validation =
                            validate_import_locator(locator_kind, activation_path);
                        const json locator{
                            {"schema", "arena_saved_locator_v1"},
                            {"schema_version", 1},
                            {"kind", locator_kind},
                            {"mode", "in_place_reference"},
                            {"path", activation_path.string()},
                            {"validation", validation},
                            {"saved_at", current_utc_iso()},
                        };
                        write_json_atomic(locator_path, locator);
                        journal["actions"].back()["created_knowledge_locator"] =
                            locator_path.string();
                    } else {
                        static_cast<void>(
                            saved_locator_path(paths.data_root, locator_kind));
                    }
                }
                journal["actions"].back()["state"] = "knowledge_activated";
                write_json_atomic(journal_path, journal);
            }

            const fs::path receipt_path = receipts_root / (id + ".json");
            if (fs::is_regular_file(receipt_path)) {
                const fs::path receipt_backup =
                    receipts_root / (id + ".rollback-" + transaction_id + ".json");
                fs::copy_file(receipt_path, receipt_backup, fs::copy_options::overwrite_existing);
                journal["actions"].back()["receipt_backup"] = receipt_backup.string();
            }
            journal["actions"].back()["receipt_path"] = receipt_path.string();
            json data_paths = json::array();
            std::string configuration_path;
            if (id == "book_forge.service" || id == "uci_arena_evidence.service" ||
                id == "lichess.adapter") {
                const fs::path workspace =
                    paths.workspace_root / (id == "book_forge.service" ? "book-forge"
                                            : id == "uci_arena_evidence.service"
                                                ? "evidence-service"
                                                : "lichess-adapter");
                if (action.value("enabled", true)) {
                    fs::create_directories(workspace);
                    const fs::path config_path = workspace / "manager-config.json";
                    journal["actions"].back()["configuration_path"] = config_path.string();
                    if (fs::is_regular_file(config_path)) {
                        const fs::path backup =
                            workspace / ("manager-config.rollback-" + transaction_id + ".json");
                        fs::copy_file(config_path, backup, fs::copy_options::overwrite_existing);
                        journal["actions"].back()["configuration_backup"] = backup.string();
                    }
                    const json generated_config =
                        generated_component_config(id, receipts_root, workspace, target);
                    if (const auto active_pointer =
                            generated_evidence_active_model_pointer(generated_config);
                        active_pointer.has_value()) {
                        const fs::path pointer_path =
                            generated_config.at("activeModelPointer").get<std::string>();
                        if (!within(workspace, pointer_path)) {
                            throw std::runtime_error(
                                "generated Evidence active-model pointer escapes its "
                                "workspace");
                        }
                        fs::create_directories(pointer_path.parent_path());
                        journal["actions"].back()["active_model_pointer_path"] =
                            pointer_path.string();
                        if (fs::is_regular_file(pointer_path)) {
                            const fs::path backup =
                                pointer_path.parent_path() /
                                ("active_model_runtime.rollback-" + transaction_id + ".json");
                            fs::copy_file(pointer_path, backup,
                                          fs::copy_options::overwrite_existing);
                            journal["actions"].back()["active_model_pointer_backup"] =
                                backup.string();
                        }
                        write_json_atomic(journal_path, journal);
                        write_json_atomic(pointer_path, *active_pointer);
                    }
                    write_json_atomic(config_path, generated_config);
                    configuration_path = config_path.string();
                }
                data_paths.push_back(workspace.string());
            }
            json receipt{
                {"schema", "arena_install_receipt_v1"},
                {"schema_version", 1},
                {"component_id", id},
                {"component_version", version},
                {"state",
                 action.value("enabled", true) ? "installed_managed" : "installed_disabled"},
                {"active_root", target.string()},
                {"artifact_sha256", action.at("artifact").at("sha256")},
                {"installed_at", current_utc_iso()},
                {"dependencies", action.value("dependencies", json::array())},
                {"data_paths", std::move(data_paths)},
                {"configuration_path", configuration_path},
                {"activated_files", knowledge_activation.value("activated_files", json::array())},
            };
            if (!paths.bundle_owner.empty()) {
                receipt["bundle_owner"] = paths.bundle_owner;
            }
            write_json_atomic(receipt_path, receipt);
            if (action.value("enabled", true) && id == "book_forge.service" &&
                saved_locator_path(paths.data_root, "lichess_token_path").has_value()) {
                receipt["credential_recovery"] =
                    invoke_book_forge_retry_auth(paths, receipt);
                write_json_atomic(receipt_path, receipt);
            }
            if (action.value("enabled", true) && id != "lichess.adapter" &&
                supports_startup_capability(receipt, "startup_install")) {
                const json startup = invoke_provider_startup(paths, receipt, "install");
                journal["actions"].back()["startup_installed"] = true;
                write_json_atomic(journal_path, journal);
                receipt["startup"] = startup;
                write_json_atomic(receipt_path, receipt);
            }
            journal["actions"].back()["state"] = "committed";
            write_json_atomic(journal_path, journal);
            results.push_back(receipt);
        }
        if (paths.restart_manager_service) {
            const int restart_result =
                arena::application::perform_service_action("UCIArenaManager", "restart");
            if (restart_result != 0) {
                throw std::runtime_error("Manager service restart did not succeed");
            }
            journal["manager_service_restarted"] = true;
        }
        journal["state"] = "committed";
        write_json_atomic(journal_path, journal);
        std::error_code ignored;
        for (const auto& action : journal.at("actions")) {
            if (action.contains("rollback_path")) {
                fs::remove_all(action.at("rollback_path").get<std::string>(), ignored);
                ignored.clear();
            }
            if (action.contains("receipt_backup")) {
                fs::remove(action.at("receipt_backup").get<std::string>(), ignored);
                ignored.clear();
            }
            if (action.contains("knowledge_activation")) {
                const std::string backup =
                    action.at("knowledge_activation").value("backup_path", "");
                if (!backup.empty()) {
                    fs::remove_all(backup, ignored);
                    ignored.clear();
                }
            }
            if (action.contains("configuration_backup")) {
                fs::remove(action.at("configuration_backup").get<std::string>(), ignored);
                ignored.clear();
            }
            if (action.contains("active_model_pointer_backup")) {
                fs::remove(action.at("active_model_pointer_backup").get<std::string>(), ignored);
                ignored.clear();
            }
        }
        fs::remove_all(staging_root / transaction_id, ignored);
    } catch (const std::exception& error) {
        std::error_code rollback_error;
        std::string startup_rollback_failure;
        for (auto action = journal["actions"].rbegin(); action != journal["actions"].rend();
             ++action) {
            const std::string state = action->value("state", "staging");
            bool preserve_action = false;
            if (action->value("startup_installed", false) && action->contains("receipt_path")) {
                try {
                    const fs::path receipt_path = action->at("receipt_path").get<std::string>();
                    if (fs::is_regular_file(receipt_path)) {
                        invoke_provider_startup(paths, read_json(receipt_path), "uninstall");
                    }
                    (*action)["startup_installed"] = false;
                } catch (const std::exception& startup_error) {
                    preserve_action = true;
                    startup_rollback_failure = startup_error.what();
                    (*action)["startup_rollback_error"] = startup_rollback_failure;
                    (*action)["state"] = "rollback_failed";
                }
            }
            if (preserve_action) {
                continue;
            }
            if (action->contains("knowledge_activation")) {
                const auto& activation = action->at("knowledge_activation");
                const fs::path destination = activation.at("path").get<std::string>();
                fs::remove_all(destination, rollback_error);
                rollback_error.clear();
                const std::string backup = activation.value("backup_path", "");
                if (!backup.empty() && fs::exists(backup)) {
                    fs::rename(backup, destination, rollback_error);
                    rollback_error.clear();
                }
            }
            if (action->contains("created_knowledge_locator")) {
                fs::remove(
                    action->at("created_knowledge_locator").get<std::string>(),
                    rollback_error);
                rollback_error.clear();
            }
            if (state != "staging" && action->contains("target")) {
                fs::remove_all(action->at("target").get<std::string>(), rollback_error);
                rollback_error.clear();
            }
            if (action->contains("rollback_path") && action->contains("target")) {
                const fs::path rollback = action->at("rollback_path").get<std::string>();
                const fs::path target = action->at("target").get<std::string>();
                if (fs::exists(rollback) && !fs::exists(target)) {
                    fs::rename(rollback, target, rollback_error);
                    rollback_error.clear();
                }
            }
            if (action->contains("receipt_path")) {
                const fs::path receipt = action->at("receipt_path").get<std::string>();
                fs::remove(receipt, rollback_error);
                rollback_error.clear();
                if (action->contains("receipt_backup")) {
                    const fs::path backup = action->at("receipt_backup").get<std::string>();
                    if (fs::exists(backup)) {
                        fs::rename(backup, receipt, rollback_error);
                        rollback_error.clear();
                    }
                }
            }
            if (action->contains("configuration_path")) {
                const fs::path configuration = action->at("configuration_path").get<std::string>();
                fs::remove(configuration, rollback_error);
                rollback_error.clear();
                if (action->contains("configuration_backup")) {
                    const fs::path backup = action->at("configuration_backup").get<std::string>();
                    if (fs::exists(backup)) {
                        fs::rename(backup, configuration, rollback_error);
                        rollback_error.clear();
                    }
                }
            }
            if (action->contains("active_model_pointer_path")) {
                const fs::path pointer = action->at("active_model_pointer_path").get<std::string>();
                fs::remove(pointer, rollback_error);
                rollback_error.clear();
                if (action->contains("active_model_pointer_backup")) {
                    const fs::path backup =
                        action->at("active_model_pointer_backup").get<std::string>();
                    if (fs::exists(backup)) {
                        fs::rename(backup, pointer, rollback_error);
                        rollback_error.clear();
                    }
                }
            }
            (*action)["state"] = "rolled_back";
        }
        fs::remove_all(staging_root / transaction_id, rollback_error);
        journal["state"] = startup_rollback_failure.empty() ? "rolled_back" : "rollback_failed";
        journal["error"] = startup_rollback_failure.empty()
                               ? error.what()
                               : std::string(error.what()) +
                                     "; startup rollback failed: " + startup_rollback_failure;
        write_json_atomic(journal_path, journal);
        if (!startup_rollback_failure.empty()) {
            throw std::runtime_error(journal.at("error").get<std::string>());
        }
        throw;
    }
    return {
        {"schema", "arena_install_apply_result_v1"},
        {"transaction_id", transaction_id},
        {"state", "committed"},
        {"components", std::move(results)},
    };
}

json SetupEngine::verify(const SetupPaths& paths) {
    json results = json::array();
    bool ok = true;
    for (const auto& receipt : receipt_rows(paths)) {
        if (!receipt.contains("component_id") || !receipt.contains("active_root")) {
            ok = false;
            results.push_back({{"component_id", receipt.value("component_id", "unknown")},
                               {"state", "broken"},
                               {"error", receipt.value("error", "receipt is incomplete")}});
            continue;
        }
        try {
            validate_component_payload(receipt.at("active_root").get<std::string>(),
                                       receipt.at("component_id").get<std::string>(),
                                       receipt.at("component_version").get<std::string>());
            for (const auto& activated : receipt.value("activated_files", json::array())) {
                const fs::path path = activated.at("path").get<std::string>();
                if (!within(paths.knowledge_root, path) || !fs::is_regular_file(path) ||
                    fs::file_size(path) != integer_field(activated, "size_bytes") ||
                    sha256_file(path) != activated.at("sha256").get<std::string>()) {
                    throw std::runtime_error("activated knowledge file verification failed: " +
                                             path.string());
                }
            }
            const std::string configuration_path = receipt.value("configuration_path", "");
            if (!configuration_path.empty()) {
                const fs::path configuration = normalized_absolute(configuration_path);
                if (!within(paths.workspace_root, configuration) ||
                    !fs::is_regular_file(configuration) || contains_symbolic_link(configuration)) {
                    throw std::runtime_error("generated component configuration is unavailable");
                }
                const json expected = generated_component_config(
                    receipt.at("component_id").get<std::string>(), paths.data_root / "receipts",
                    configuration.parent_path(), receipt.at("active_root").get<std::string>());
                if (read_json(configuration) != expected) {
                    throw std::runtime_error("generated component configuration drifted from its "
                                             "verified installer inputs");
                }
                if (const auto expected_pointer = generated_evidence_active_model_pointer(expected);
                    expected_pointer.has_value()) {
                    const fs::path pointer = expected.at("activeModelPointer").get<std::string>();
                    if (!within(configuration.parent_path(), pointer) ||
                        !fs::is_regular_file(pointer) || contains_symbolic_link(pointer) ||
                        read_json(pointer) != *expected_pointer) {
                        throw std::runtime_error("generated Evidence active-model pointer drifted "
                                                 "from its verified installer inputs");
                    }
                }
            }
            if (receipt.contains("startup") && receipt.at("startup").is_object() &&
                receipt.at("startup").value("installed", false)) {
                invoke_provider_startup(paths, receipt, "status");
            }
            results.push_back(
                {{"component_id", receipt.at("component_id")}, {"state", "verified"}});
        } catch (const std::exception& error) {
            ok = false;
            results.push_back({{"component_id", receipt.at("component_id")},
                               {"state", "broken"},
                               {"error", error.what()}});
        }
    }
    return {
        {"schema", "arena_install_verify_result_v1"},
        {"ok", ok},
        {"components", std::move(results)},
    };
}

json SetupEngine::uninstall(const SetupPaths& paths, const std::vector<std::string>& component_ids,
                            const bool confirmed, const bool purge, const bool all) {
    if (!confirmed) {
        throw std::runtime_error("uninstall requires explicit confirmation");
    }
    if (!paths.bundle_owner.empty() && !valid_bundle_owner(paths.bundle_owner)) {
        throw std::runtime_error("bundle owner is invalid");
    }
    std::map<std::string, json, std::less<>> installed;
    for (const auto& receipt : receipt_rows(paths)) {
        if (receipt.value("schema", "") == "arena_install_receipt_v1" &&
            receipt.contains("component_id") && receipt.at("component_id").is_string()) {
            installed.emplace(receipt.at("component_id").get<std::string>(), receipt);
        }
    }
    std::vector<std::string> requested = component_ids;
    std::size_t skipped_foreign_receipts = 0;
    if (all) {
        requested.clear();
        for (const auto& [component_id, receipt] : installed) {
            if (!paths.bundle_owner.empty() &&
                receipt.value("bundle_owner", "") != paths.bundle_owner) {
                ++skipped_foreign_receipts;
                continue;
            }
            requested.push_back(component_id);
        }
    }
    if (!paths.bundle_owner.empty() && !all) {
        std::erase_if(requested, [&installed, &paths,
                                  &skipped_foreign_receipts](const std::string& component_id) {
            const auto receipt = installed.find(component_id);
            if (receipt == installed.end() ||
                receipt->second.value("bundle_owner", "") == paths.bundle_owner) {
                return false;
            }
            ++skipped_foreign_receipts;
            return true;
        });
    }
    if (requested.empty() && paths.bundle_owner.empty()) {
        throw std::runtime_error("uninstall requires at least one component or --all");
    }
    std::set<std::string, std::less<>> selected;
    std::vector<std::string> unique;
    for (const auto& id : requested) {
        if (!valid_identifier(id)) {
            throw std::runtime_error("invalid component id " + id);
        }
        if (selected.insert(id).second) {
            unique.push_back(id);
        }
    }
    for (const auto& [consumer_id, receipt] : installed) {
        if (selected.contains(consumer_id)) {
            continue;
        }
        const json dependencies = receipt.value("dependencies", json::array());
        if (!dependencies.is_array()) {
            throw std::runtime_error("installed component dependencies are invalid: " +
                                     consumer_id);
        }
        for (const auto& dependency : dependencies) {
            if (dependency.is_string() && selected.contains(dependency.get<std::string>())) {
                throw std::runtime_error("cannot uninstall dependency " +
                                         dependency.get<std::string>() + " while " + consumer_id +
                                         " remains installed");
            }
        }
    }
    const auto uninstall_priority = [&installed](const std::string& id) {
        const auto receipt = installed.find(id);
        if (receipt != installed.end() &&
            supports_startup_capability(receipt->second, "startup_uninstall")) {
            return 0;
        }
        if (id == "node_runtime.private") {
            return 2;
        }
        if (id == "manager.core") {
            return 3;
        }
        return 1;
    };
    std::ranges::stable_sort(
        unique, [&uninstall_priority](const std::string& left, const std::string& right) {
            return uninstall_priority(left) < uninstall_priority(right);
        });
    std::map<std::string, int, std::less<>> visits;
    std::vector<std::string> ordered;
    std::function<void(const std::string&)> add = [&](const std::string& id) {
        if (visits[id] == 1) {
            throw std::runtime_error("installed component dependency cycle at " + id);
        }
        if (visits[id] == 2) {
            return;
        }
        visits[id] = 1;
        for (const auto& [candidate_id, receipt] : installed) {
            if (!selected.contains(candidate_id)) {
                continue;
            }
            const json dependencies = receipt.value("dependencies", json::array());
            if (std::find(dependencies.begin(), dependencies.end(), id) != dependencies.end()) {
                add(candidate_id);
            }
        }
        visits[id] = 2;
        ordered.push_back(id);
    };
    for (const auto& id : unique) {
        add(id);
    }

    json results = json::array();
    for (const auto& id : ordered) {
        const fs::path receipt_path = paths.data_root / "receipts" / (id + ".json");
        if (!fs::is_regular_file(receipt_path)) {
            results.push_back({{"component_id", id}, {"state", "not_installed"}});
            continue;
        }
        const json receipt = read_json(receipt_path);
        const fs::path active_root = receipt.at("active_root").get<std::string>();
        if (!within(paths.install_root / "components", active_root)) {
            throw std::runtime_error("receipt active root is outside the managed component root");
        }
        json startup_cleanup;
        if (supports_startup_capability(receipt, "startup_uninstall")) {
            startup_cleanup = invoke_provider_startup(paths, receipt, "uninstall");
        }
        remove_managed_component_tree(active_root, id);
        const fs::path component_directory = active_root.parent_path();
        const fs::path components_root = paths.install_root / "components";
        std::error_code component_directory_error;
        if (within(components_root, component_directory) &&
            fs::is_directory(component_directory, component_directory_error) &&
            fs::is_empty(component_directory, component_directory_error)) {
            fs::remove(component_directory, component_directory_error);
        }
        if (component_directory_error) {
            throw std::runtime_error("cannot remove empty component directory for " + id);
        }
        std::error_code error;
        if (purge) {
            for (const auto& data_path_value : receipt.value("data_paths", json::array())) {
                const fs::path data_path = data_path_value.get<std::string>();
                if (!within(paths.data_root, data_path) &&
                    !within(paths.workspace_root, data_path)) {
                    throw std::runtime_error("purge path is outside the managed data root");
                }
                fs::remove_all(data_path, error);
                if (error) {
                    throw std::runtime_error("cannot purge managed data for " + id);
                }
            }
            for (const auto& activated : receipt.value("activated_files", json::array())) {
                const fs::path activated_path = activated.at("path").get<std::string>();
                if (!within(paths.knowledge_root, activated_path)) {
                    throw std::runtime_error(
                        "knowledge purge path is outside the managed knowledge root");
                }
                if (fs::is_regular_file(activated_path) &&
                    sha256_file(activated_path) == activated.at("sha256").get<std::string>()) {
                    fs::remove(activated_path, error);
                    if (error) {
                        throw std::runtime_error("cannot purge managed knowledge for " + id);
                    }
                }
            }
        }
        fs::remove(receipt_path);
        json result{
            {"component_id", id},
            {"state", "uninstalled"},
            {"data_preserved", !purge},
        };
        if (!startup_cleanup.empty()) {
            result["startup_cleanup"] = std::move(startup_cleanup);
        }
        results.push_back(std::move(result));
    }
    json result{
        {"schema", "arena_install_uninstall_result_v1"},
        {"all", all},
        {"skipped_foreign_receipts", skipped_foreign_receipts},
        {"components", std::move(results)},
    };
    if (!paths.bundle_owner.empty()) {
        result["bundle_owner"] = paths.bundle_owner;
    }
    return result;
}

json copy_managed_lichess_profile(const SetupPaths& paths, const fs::path& source,
                                  const json& validation) {
    const auto files = lichess_single_game_profile_files(source);
    json identity_rows = json::array();
    for (const auto& file : files) {
        identity_rows.push_back({{"path", file.relative.generic_string()},
                                 {"size_bytes", file.size},
                                 {"sha256", file.sha256}});
    }
    const std::string fingerprint = sha256_text(identity_rows.dump());
    const std::string component_id = "lichess_profile.import." + fingerprint.substr(0, 16);
    const std::string component_version = "1";
    const fs::path import_root = paths.data_root / "imports" / "lichess_profiles";
    const fs::path active_root = import_root / fingerprint;
    const fs::path staging_root = import_root / (".staging-" + fingerprint);
    const bool active_root_existed = fs::exists(active_root);
    fs::create_directories(import_root);
    require_no_symbolic_link(import_root);

    std::error_code ignored;
    fs::remove_all(staging_root, ignored);
    if (!fs::exists(active_root)) {
        fs::create_directories(staging_root);
        try {
            for (const auto& file : files) {
                const fs::path target = staging_root / file.relative;
                if (!within(staging_root, target)) {
                    throw std::runtime_error("managed Lichess profile path escapes staging");
                }
                fs::create_directories(target.parent_path());
                fs::copy_file(file.source, target, fs::copy_options::overwrite_existing);
                if (fs::file_size(target) != file.size ||
                    sha256_file(target) != file.sha256) {
                    throw std::runtime_error(
                        "managed Lichess profile closure verification failed");
                }
            }
            const json manifest{
                {"schema", "arena_install_component_v1"},
                {"schema_version", 1},
                {"component_id", component_id},
                {"component_version", component_version},
                {"entrypoints",
                 {{"profile", source.filename().generic_string()}}},
                {"files", identity_rows},
                {"capabilities", {"qualified_lichess_profile"}},
                {"uninstall_policy", "preserve_with_manager_data"},
            };
            write_json_atomic(staging_root / "arena-component.json", manifest);
            fs::rename(staging_root, active_root);
        } catch (...) {
            fs::remove_all(staging_root, ignored);
            throw;
        }
    }
    const fs::path managed_profile = active_root / source.filename();
    if (!fs::is_regular_file(managed_profile) ||
        sha256_file(managed_profile) != sha256_file(source)) {
        throw std::runtime_error("managed Lichess profile activation verification failed");
    }

    const fs::path receipt_path =
        paths.data_root / "receipts" / (component_id + ".json");
    const bool receipt_existed = fs::is_regular_file(receipt_path);
    const json receipt{
        {"schema", "arena_install_receipt_v1"},
        {"schema_version", 1},
        {"component_id", component_id},
        {"component_version", component_version},
        {"state", "installed_managed"},
        {"active_root", active_root.string()},
        {"artifact_sha256", fingerprint},
        {"installed_at", current_utc_iso()},
        {"dependencies", json::array()},
        {"data_paths", json::array()},
        {"configuration_path", ""},
        {"activated_files", json::array()},
    };
    write_json_atomic(receipt_path, receipt);
    return {
        {"schema", "arena_setup_import_result_v1"},
        {"schema_version", 1},
        {"kind", "lichess_single_game_profile"},
        {"mode", "managed_copy"},
        {"state", "installed_managed"},
        {"path", managed_profile.string()},
        {"validation", validation},
        {"receipt", receipt},
        {"_active_root_existed", active_root_existed},
        {"_receipt_existed", receipt_existed},
    };
}

} // namespace arena::application::installer
