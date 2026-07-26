#include "arena/application/installer/setup.hpp"

#include <archive.h>
#include <archive_entry.h>
#include <curl/curl.h>
#include <sodium.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cctype>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <stdexcept>
#include <system_error>

#ifndef _WIN32
#include <sys/stat.h>
#endif

namespace arena::application::installer {
namespace {

using nlohmann::json;
namespace fs = std::filesystem;

constexpr std::uint64_t gib = 1024ULL * 1024ULL * 1024ULL;
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
            crypto_hash_sha256_update(
                &state, buffer.data(), static_cast<unsigned long long>(count));
        }
    }
    std::array<unsigned char, crypto_hash_sha256_BYTES> digest{};
    crypto_hash_sha256_final(&state, digest.data());
    std::array<char, crypto_hash_sha256_BYTES * 2 + 1> encoded{};
    sodium_bin2hex(encoded.data(), encoded.size(), digest.data(), digest.size());
    return encoded.data();
}

bool valid_identifier(const std::string& value) {
    return !value.empty() &&
           std::all_of(value.begin(), value.end(), [](const unsigned char character) {
               return std::isalnum(character) != 0 || character == '.' ||
                      character == '_' || character == '-';
           });
}

fs::path normalized_absolute(const fs::path& value) {
    return fs::absolute(value).lexically_normal();
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
    std::string key =
        (existing.root_name() / existing.root_directory()).string();
    std::ranges::transform(
        key, key.begin(), [](const unsigned char character) {
            return static_cast<char>(std::tolower(character));
        });
    return key;
#else
    struct stat status {};
    if (::stat(existing.c_str(), &status) != 0) {
        throw std::runtime_error(
            "cannot identify filesystem volume for " + existing.string());
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

std::vector<unsigned char> decode_hex(
    const std::string& encoded,
    const std::size_t expected_size,
    const std::string& label) {
    std::vector<unsigned char> decoded(expected_size);
    std::size_t actual = 0;
    if (sodium_hex2bin(
            decoded.data(),
            decoded.size(),
            encoded.c_str(),
            encoded.size(),
            nullptr,
            &actual,
            nullptr) != 0 ||
        actual != expected_size) {
        throw std::runtime_error("invalid hexadecimal " + label);
    }
    return decoded;
}

void verify_role(
    const json& metadata,
    const json& root_signed,
    const std::string& role_name) {
    if (!metadata.contains("signed") || !metadata.contains("signatures") ||
        !metadata.at("signatures").is_array()) {
        throw std::runtime_error("TUF " + role_name + " metadata is incomplete");
    }
    const auto& role = root_signed.at("roles").at(role_name);
    const std::set<std::string> allowed(
        role.at("keyids").begin(), role.at("keyids").end());
    const std::size_t threshold = role.at("threshold").get<std::size_t>();
    const std::string canonical = metadata.at("signed").dump();
    std::set<std::string> accepted;
    for (const auto& signature : metadata.at("signatures")) {
        const std::string key_id = signature.value("keyid", "");
        if (!allowed.contains(key_id) || accepted.contains(key_id)) {
            continue;
        }
        const auto& key = root_signed.at("keys").at(key_id);
        if (key.value("keytype", "") != "ed25519" ||
            key.value("scheme", "") != "ed25519") {
            continue;
        }
        const auto public_key = decode_hex(
            key.at("keyval").at("public").get<std::string>(),
            crypto_sign_PUBLICKEYBYTES,
            "TUF public key");
        const auto signature_bytes = decode_hex(
            signature.at("sig").get<std::string>(),
            crypto_sign_BYTES,
            "TUF signature");
        if (crypto_sign_verify_detached(
                signature_bytes.data(),
                reinterpret_cast<const unsigned char*>(canonical.data()),
                canonical.size(),
                public_key.data()) == 0) {
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

void verify_described_file(
    const fs::path& path,
    const json& descriptor,
    const std::string& label) {
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

void verify_tuf_chain(
    const fs::path& catalog_path,
    const fs::path& metadata_directory,
    const fs::path& rollback_state_path) {
    if (sodium_init() < 0) {
        throw std::runtime_error("libsodium initialization failed");
    }
    const fs::path root_path = metadata_directory / "root.json";
    const std::string pinned_root = compiled_tuf_root_sha256;
    if (pinned_root.size() != 64 ||
        sha256_file(root_path) != pinned_root) {
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
    verify_described_file(
        metadata_directory / "snapshot.json",
        timestamp.at("signed").at("meta").at("snapshot.json"),
        "TUF snapshot");

    const json snapshot = read_json(metadata_directory / "snapshot.json");
    verify_role(snapshot, root.at("signed"), "snapshot");
    verify_not_expired(snapshot, "snapshot");
    verify_described_file(
        metadata_directory / "targets.json",
        snapshot.at("signed").at("meta").at("targets.json"),
        "TUF targets");

    const json targets = read_json(metadata_directory / "targets.json");
    verify_role(targets, root.at("signed"), "targets");
    verify_not_expired(targets, "targets");
    const std::string target_name = catalog_path.filename().generic_string();
    verify_described_file(
        catalog_path,
        targets.at("signed").at("targets").at(target_name),
        "installation catalog");

    const json versions{
        {"root", metadata_version(root, "root")},
        {"timestamp", metadata_version(timestamp, "timestamp")},
        {"snapshot", metadata_version(snapshot, "snapshot")},
        {"targets", metadata_version(targets, "targets")},
    };
    if (timestamp.at("signed")
            .at("meta")
            .at("snapshot.json")
            .value("version", 0ULL) !=
            versions.at("snapshot").get<std::uint64_t>() ||
        snapshot.at("signed")
            .at("meta")
            .at("targets.json")
            .value("version", 0ULL) !=
            versions.at("targets").get<std::uint64_t>()) {
        throw std::runtime_error("TUF delegated metadata versions disagree");
    }
    if (!rollback_state_path.empty() &&
        fs::is_regular_file(rollback_state_path)) {
        const json previous = read_json(rollback_state_path);
        if (previous.value("schema", "") !=
            "arena_tuf_rollback_state_v1") {
            throw std::runtime_error("TUF rollback state is malformed");
        }
        for (const auto& [role, version] : versions.items()) {
            if (version.get<std::uint64_t>() <
                previous.value(role, 0ULL)) {
                throw std::runtime_error(
                    "TUF rollback detected for " + role);
            }
        }
    }
    if (!rollback_state_path.empty()) {
        write_json_atomic(
            rollback_state_path,
            {
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
    throw std::runtime_error(
        "component " + component.at("id").get<std::string>() +
        " has no artifact for " + key);
}

std::size_t curl_write(char* data, std::size_t size, std::size_t count, void* output) {
    static_cast<std::ofstream*>(output)->write(
        data, static_cast<std::streamsize>(size * count));
    return size * count;
}

fs::path acquire_artifact(
    const json& artifact,
    const fs::path& catalog_directory,
    const SetupPaths& paths,
    const bool offline) {
    fs::create_directories(paths.cache_root);
    const std::string expected_hash = artifact.at("sha256").get<std::string>();
    if (expected_hash.size() != 64) {
        throw std::runtime_error("artifact SHA-256 is malformed");
    }
    const fs::path cached = paths.cache_root / expected_hash;
    if (fs::is_regular_file(cached) && sha256_file(cached) == expected_hash) {
        return cached;
    }

    const std::string source = artifact.at("source").get<std::string>();
    const fs::path temporary = cached.string() + ".partial";
    std::error_code ignored;
    fs::remove(temporary, ignored);
    if (source.starts_with("https://")) {
        if (offline) {
            throw std::runtime_error("offline mode cannot download " + source);
        }
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
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
        curl_easy_setopt(handle, CURLOPT_WRITEFUNCTION, curl_write);
        curl_easy_setopt(handle, CURLOPT_WRITEDATA, &output);
        const CURLcode result = curl_easy_perform(handle);
        curl_easy_cleanup(handle);
        output.close();
        if (result != CURLE_OK) {
            fs::remove(temporary, ignored);
            throw std::runtime_error(
                "artifact download failed: " + std::string(curl_easy_strerror(result)));
        }
    } else {
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
                raw_path == nullptr
                    ? fs::path{}
                    : fs::path(raw_path).lexically_normal();
            if (relative == fs::path(".") &&
                archive_entry_filetype(entry) == AE_IFDIR) {
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
            const int result = archive_read_extract(
                reader,
                entry,
                ARCHIVE_EXTRACT_TIME | ARCHIVE_EXTRACT_PERM |
                    ARCHIVE_EXTRACT_SECURE_NODOTDOT |
                    ARCHIVE_EXTRACT_SECURE_SYMLINKS);
            if (result != ARCHIVE_OK) {
                throw std::runtime_error(
                    "component extraction failed: " +
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

json validate_component_payload(
    const fs::path& root,
    const std::string& expected_id,
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
            throw std::runtime_error(
                "component payload verification failed for " + relative.generic_string());
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
        if (relative != fs::path("arena-component.json") &&
            !listed_files.contains(relative)) {
            throw std::runtime_error(
                "component payload contains an unlisted file: " +
                relative.generic_string());
        }
    }
    return manifest;
}

void copy_directory_contents(
    const fs::path& source,
    const fs::path& destination,
    std::vector<fs::path>& copied_files) {
    if (!fs::is_directory(source)) {
        throw std::runtime_error(
            "knowledge component is missing " + source.filename().string());
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
            fs::copy_file(
                entry.path(), target, fs::copy_options::overwrite_existing);
            copied_files.push_back(relative);
        } else {
            throw std::runtime_error("knowledge payload contains an unsupported entry");
        }
    }
}

json activate_knowledge(
    const fs::path& component_root,
    const json& manifest,
    const SetupPaths& paths,
    const std::string& transaction_id,
    const std::string& component_id) {
    const auto capabilities =
        manifest.value("capabilities", std::vector<std::string>{});
    std::optional<std::pair<fs::path, fs::path>> mapping;
    if (std::find(capabilities.begin(), capabilities.end(), "opening_book") !=
        capabilities.end()) {
        mapping = std::pair{
            component_root / "opening_books",
            paths.knowledge_root / "opening_books"};
    } else if (
        std::find_if(
            capabilities.begin(),
            capabilities.end(),
            [](const std::string& capability) {
                return capability == "syzygy" ||
                       capability.rfind("syzygy_", 0) == 0;
            }) != capabilities.end()) {
        mapping = std::pair{
            component_root / "tablebases",
            paths.knowledge_root / "tablebases"};
    }
    if (!mapping.has_value()) {
        return json::object();
    }

    const auto& [source, destination] = *mapping;
    fs::create_directories(destination.parent_path());
    const fs::path candidate =
        destination.parent_path() /
        (destination.filename().string() + ".staging-" + transaction_id + "-" +
         component_id);
    const fs::path backup =
        destination.parent_path() /
        (destination.filename().string() + ".rollback-" + transaction_id + "-" +
         component_id);
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
    const std::string manifest_prefix =
        source.filename().generic_string() + "/";
    for (const auto& file : manifest.at("files")) {
        const std::string relative = file.at("path").get<std::string>();
        if (relative.rfind(manifest_prefix, 0) != 0) {
            continue;
        }
        activated_files.push_back(
            {{"path",
              (destination / fs::path(relative.substr(manifest_prefix.size())))
                  .string()},
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

std::vector<std::string> selected_ids(
    const json& catalog,
    const std::string& preset,
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
    if (!fs::is_directory(directory)) {
        return rows;
    }
    for (const auto& entry : fs::directory_iterator(directory)) {
        if (entry.is_regular_file() && entry.path().extension() == ".json") {
            try {
                rows.push_back(read_json(entry.path()));
            } catch (const std::exception& error) {
                rows.push_back(
                    {{"schema", "arena_install_receipt_v1"},
                     {"state", "broken"},
                     {"receipt", entry.path().string()},
                     {"error", error.what()}});
            }
        }
    }
    return rows;
}

fs::path installed_entrypoint(
    const fs::path& receipts_root,
    const std::string& component_id,
    const std::string& entrypoint_name) {
    const json receipt =
        read_json(receipts_root / (component_id + ".json"));
    if (receipt.value("schema", "") != "arena_install_receipt_v1" ||
        receipt.value("component_id", "") != component_id ||
        receipt.value("state", "") != "installed_managed") {
        throw std::runtime_error(
            "required installed dependency is unavailable: " + component_id);
    }
    const fs::path active_root =
        receipt.at("active_root").get<std::string>();
    const json manifest = read_json(active_root / "arena-component.json");
    if (manifest.value("schema", "") != "arena_install_component_v1" ||
        manifest.value("component_id", "") != component_id ||
        !manifest.contains("entrypoints") ||
        !manifest.at("entrypoints").contains(entrypoint_name)) {
        throw std::runtime_error(
            "required installed dependency entrypoint is unavailable: " +
            component_id + ":" + entrypoint_name);
    }
    const fs::path relative =
        manifest.at("entrypoints").at(entrypoint_name).get<std::string>();
    if (!safe_relative_path(relative)) {
        throw std::runtime_error(
            "installed dependency entrypoint is unsafe");
    }
    const fs::path entrypoint = active_root / relative;
    for (const auto& file : manifest.at("files")) {
        if (file.value("path", "") == relative.generic_string() &&
            fs::is_regular_file(entrypoint) &&
            !fs::is_symlink(entrypoint) &&
            fs::file_size(entrypoint) ==
                integer_field(file, "size_bytes") &&
            sha256_file(entrypoint) ==
                file.at("sha256").get<std::string>()) {
            return normalized_absolute(entrypoint);
        }
    }
    throw std::runtime_error(
        "installed dependency entrypoint digest mismatch: " +
        component_id + ":" + entrypoint_name);
}

json generated_service_config(
    const std::string& component_id,
    const fs::path& receipts_root) {
    if (component_id == "book_forge.service") {
        return {
            {"schema", "flashfork_book_forge_config_v1"},
            {"stockfish",
             installed_entrypoint(
                 receipts_root, "stockfish.engine", "stockfish").string()},
        };
    }
    if (component_id == "uci_arena_evidence.service") {
        return {
            {"schema", "uci_arena_evidence_service_config_v1"},
            {"engine",
             installed_entrypoint(
                 receipts_root, "uci_arena.engine", "uci").string()},
            {"stockfish",
             installed_entrypoint(
                 receipts_root, "stockfish.engine", "stockfish").string()},
            {"referee",
             installed_entrypoint(
                 receipts_root, "uci_arena.engine", "referee").string()},
        };
    }
    return json();
}

}  // namespace

std::string SetupEngine::platform_key() {
#if defined(_WIN32)
    return sizeof(void*) == 8 ? "windows-x86_64" : "windows-x86";
#elif defined(__aarch64__)
    return "linux-aarch64";
#else
    return "linux-x86_64";
#endif
}

json SetupEngine::load_catalog(
    const fs::path& catalog_path,
    const fs::path& metadata_directory,
    const bool allow_unsigned_development_catalog,
    const fs::path& rollback_state_path) {
    if (metadata_directory.empty()) {
        if (!allow_unsigned_development_catalog) {
            throw std::runtime_error(
                "trusted TUF metadata is required; unsigned catalogs are development-only");
        }
    } else {
        verify_tuf_chain(
            catalog_path, metadata_directory, rollback_state_path);
    }
    json catalog = read_json(catalog_path);
    if (catalog.value("schema", "") != "arena_install_catalog_v1" ||
        catalog.value("schema_version", 0) != 1 ||
        !catalog.contains("components") || !catalog.at("components").is_array() ||
        !catalog.contains("presets") || !catalog.at("presets").is_object()) {
        throw std::runtime_error("installation catalog does not satisfy arena_install_catalog_v1");
    }
    std::set<std::string> ids;
    for (const auto& component : catalog.at("components")) {
        const std::string id = component.value("id", "");
        if (!valid_identifier(id) || !ids.insert(id).second ||
            !component.contains("version") || !component.contains("artifacts")) {
            throw std::runtime_error("installation catalog has an invalid component identity");
        }
    }
    catalog["_catalog_directory"] = normalized_absolute(catalog_path.parent_path()).string();
    return catalog;
}

json SetupEngine::inventory(
    const SetupPaths& paths,
    const std::vector<fs::path>& extra_roots) {
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
        if (!fs::is_regular_file(book_root / name)) {
            knowledge["opening_book"]["missing_files"].push_back(name);
        }
    }
    if (knowledge["opening_book"]["missing_files"].empty()) {
        knowledge["opening_book"]["state"] = "detected_unmanaged";
        knowledge["opening_book"]["path"] = book_root.string();
    }
    const fs::path tablebase_root = paths.knowledge_root / "tablebases";
    if (fs::is_directory(tablebase_root)) {
        for (const auto& entry : fs::directory_iterator(tablebase_root)) {
            if (!entry.is_regular_file()) {
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
                knowledge["syzygy"]["bytes"].get<std::uint64_t>() +
                entry.file_size();
        }
        if (knowledge["syzygy"]["wdl_files"].get<std::uint64_t>() > 0) {
            knowledge["syzygy"]["state"] = "detected_unmanaged";
            knowledge["syzygy"]["path"] = tablebase_root.string();
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
        "stockfish.exe", "lc0.exe", "flashfork_uci.exe"
#else
        "stockfish", "lc0", "flashfork_uci"
#endif
    };
    std::set<std::string> seen;
    for (const auto& root : bounded_roots) {
        if (!fs::is_directory(root)) {
            continue;
        }
        for (const auto& name : candidate_names) {
            const fs::path candidate = root / name;
            if (fs::is_regular_file(candidate)) {
                const std::string normalized = normalized_absolute(candidate).string();
                if (seen.insert(normalized).second) {
                    candidates.push_back(
                        {{"kind", "uci_engine"},
                         {"state", "detected_unmanaged"},
                         {"path", normalized},
                         {"requires_probe", true}});
                }
            }
        }
    }

    json restaurant{{"state", "absent"}};
    if (const char* configured = std::getenv("RESTAURANT_ROOT");
        configured != nullptr && fs::is_directory(configured)) {
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
        {"restaurant", std::move(restaurant)},
    };
}

json SetupEngine::plan(
    const json& catalog,
    const SetupPaths& paths,
    const std::string& preset,
    const std::vector<std::string>& selected_components,
    const bool offline,
    const bool download_missing_knowledge,
    const bool download_missing_books,
    const bool download_missing_tablebases) {
    const auto ids =
        selected_ids(
            catalog,
            preset,
            selected_components,
            download_missing_knowledge,
            download_missing_books,
            download_missing_tablebases);
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
        reserve =
            (std::max)(reserve, integer_field(component, "workspace_reserve_bytes"));
        const fs::path receipt_path =
            paths.data_root / "receipts" / (id + ".json");
        if (fs::is_regular_file(receipt_path)) {
            try {
                const json receipt = read_json(receipt_path);
                if (receipt.value("component_id", "") == id &&
                    receipt.value("component_version", "") ==
                        component.at("version").get<std::string>() &&
                    (receipt.value("state", "") == "installed_managed" ||
                     receipt.value("state", "") == "installed_disabled")) {
                    validate_component_payload(
                        receipt.at("active_root").get<std::string>(),
                        id,
                        receipt.at("component_version").get<std::string>());
                    actions.push_back(
                        {{"action", "keep"},
                         {"component_id", id},
                         {"version", component.at("version")},
                         {"enabled",
                          receipt.value("state", "") ==
                              "installed_managed"}});
                    continue;
                }
            } catch (const std::exception&) {
            }
        }
        const auto& artifact = platform_artifact(component);
        const std::string hash = artifact.at("sha256").get<std::string>();
        if (artifact_hashes.insert(hash).second) {
            download += integer_field(artifact, "size_bytes");
            staging += integer_field(component, "staging_bytes");
        }
        installed += integer_field(component, "installed_bytes");
        if (id == "opening_book.standard" || id.rfind("syzygy.", 0) == 0) {
            knowledge_activation += integer_field(component, "installed_bytes");
        }
        backup += integer_field(component, "backup_bytes");
        actions.push_back(
            {{"action", "install"},
             {"component_id", id},
             {"version", component.at("version")},
             {"enabled", true},
             {"artifact", artifact}});
    }
    json blockers = json::array();
    std::uint64_t effective_reserve = reserve;
    if (preset != "complete_arena" && reserve > 0) {
        const fs::space_info workspace_space =
            fs::space(nearest_existing_ancestor(paths.workspace_root));
        if (workspace_space.available < reserve) {
            for (auto& action : actions) {
                const std::string id = action.at("component_id").get<std::string>();
                if (action.value("action", "") == "install" &&
                    (id == "book_forge.service" ||
                     id == "uci_arena_evidence.service")) {
                    action["enabled"] = false;
                    action["warning"] =
                        "installed disabled until workspace reserve is available";
                }
            }
            effective_reserve = 0;
        }
    }
    const std::uint64_t headroom = (std::max)(gib, staging / 10);
    std::map<std::string, json> volumes;
    const auto allocate =
        [&volumes](
            const fs::path& path,
            const std::string& role,
            const char* field,
            const std::uint64_t bytes) {
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
            if (std::find(
                    volume["roles"].begin(),
                    volume["roles"].end(),
                    role) == volume["roles"].end()) {
                volume["roles"].push_back(role);
            }
            volume[field] =
                volume.at(field).get<std::uint64_t>() + bytes;
            volume["peak_required_bytes"] =
                volume.at("peak_required_bytes").get<std::uint64_t>() + bytes;
        };
    allocate(
        paths.cache_root, "download_cache", "download_cache_bytes", download);
    allocate(paths.install_root, "staging", "staging_bytes", staging);
    allocate(paths.install_root, "components", "installed_bytes", installed);
    allocate(paths.install_root, "rollback", "backup_bytes", backup);
    allocate(paths.install_root, "staging_headroom", "headroom_bytes", headroom);
    allocate(
        paths.knowledge_root,
        "knowledge_activation",
        "installed_bytes",
        knowledge_activation);
    allocate(
        paths.workspace_root,
        "operational_workspace",
        "operational_reserve_bytes",
        effective_reserve);

    json volume_rows = json::array();
    std::uint64_t peak = 0;
    for (auto& [key, volume] : volumes) {
        (void)key;
        const auto required =
            volume.at("peak_required_bytes").get<std::uint64_t>();
        const auto available =
            volume.at("available_bytes").get<std::uint64_t>();
        peak += required;
        if (available < required) {
            const bool workspace_volume =
                std::find(
                    volume["roles"].begin(),
                    volume["roles"].end(),
                    "operational_workspace") != volume["roles"].end();
            blockers.push_back(
                {{"code",
                  preset == "complete_arena" && workspace_volume ?
                      "workspace_reserve_unmet" :
                      "insufficient_space"},
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
        {"download_missing_knowledge", download_missing_knowledge},
        {"download_missing_books",
         download_missing_knowledge || download_missing_books},
        {"download_missing_tablebases",
         download_missing_knowledge || download_missing_tablebases},
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

json SetupEngine::apply(
    const json&,
    const json& plan,
    const SetupPaths& paths,
    const bool confirmed,
    const bool repair) {
    if (!confirmed) {
        throw std::runtime_error("apply requires explicit confirmation");
    }
    if (plan.value("schema", "") != "arena_install_plan_v1" ||
        !plan.at("blockers").empty()) {
        throw std::runtime_error("installation plan is invalid or blocked");
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
        const fs::path catalog_directory =
            fs::path(plan.value("catalog_directory", "."));
        for (const auto& action : plan.at("actions")) {
            if (action.value("action", "install") == "keep") {
                results.push_back(
                    {{"component_id", action.at("component_id")},
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
                {{"component_id", id},
                 {"version", version},
                 {"state", "staging"}});
            write_json_atomic(journal_path, journal);
            const fs::path artifact = acquire_artifact(
                action.at("artifact"),
                catalog_directory,
                paths,
                plan.value("offline", false));
            const fs::path stage = staging_root / transaction_id / id;
            std::error_code ignored;
            fs::remove_all(stage, ignored);
            extract_archive(artifact, stage);
            const json manifest = validate_component_payload(stage, id, version);

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
                    component_root /
                    (version + ".rollback-" + transaction_id);
                fs::rename(target, replaced);
                journal["actions"].back()["rollback_path"] = replaced.string();
            }
            fs::rename(stage, target);
            journal["actions"].back()["state"] = "target_activated";
            write_json_atomic(journal_path, journal);

            const json knowledge_activation =
                activate_knowledge(target, manifest, paths, transaction_id, id);
            if (!knowledge_activation.empty()) {
                journal["actions"].back()["knowledge_activation"] =
                    knowledge_activation;
                journal["actions"].back()["state"] = "knowledge_activated";
                write_json_atomic(journal_path, journal);
            }

            const fs::path receipt_path = receipts_root / (id + ".json");
            if (fs::is_regular_file(receipt_path)) {
                const fs::path receipt_backup =
                    receipts_root /
                    (id + ".rollback-" + transaction_id + ".json");
                fs::copy_file(
                    receipt_path,
                    receipt_backup,
                    fs::copy_options::overwrite_existing);
                journal["actions"].back()["receipt_backup"] =
                    receipt_backup.string();
            }
            journal["actions"].back()["receipt_path"] = receipt_path.string();
            json data_paths = json::array();
            std::string configuration_path;
            if (id == "book_forge.service" ||
                id == "uci_arena_evidence.service") {
                const fs::path workspace =
                    paths.workspace_root /
                    (id == "book_forge.service" ? "book-forge" :
                                                   "evidence-service");
                if (action.value("enabled", true)) {
                    fs::create_directories(workspace);
                    const fs::path config_path =
                        workspace / "manager-config.json";
                    journal["actions"].back()["configuration_path"] =
                        config_path.string();
                    if (fs::is_regular_file(config_path)) {
                        const fs::path backup =
                            workspace /
                            ("manager-config.rollback-" +
                             transaction_id + ".json");
                        fs::copy_file(
                            config_path,
                            backup,
                            fs::copy_options::overwrite_existing);
                        journal["actions"].back()["configuration_backup"] =
                            backup.string();
                    }
                    write_json_atomic(
                        config_path,
                        generated_service_config(id, receipts_root));
                    configuration_path = config_path.string();
                }
                data_paths.push_back(workspace.string());
            }
            const json receipt{
                {"schema", "arena_install_receipt_v1"},
                {"schema_version", 1},
                {"component_id", id},
                {"component_version", version},
                {"state",
                 action.value("enabled", true) ? "installed_managed" :
                                                 "installed_disabled"},
                {"active_root", target.string()},
                {"artifact_sha256", action.at("artifact").at("sha256")},
                {"installed_at", current_utc_iso()},
                {"data_paths", std::move(data_paths)},
                {"configuration_path", configuration_path},
                {"activated_files",
                 knowledge_activation.value("activated_files", json::array())},
            };
            write_json_atomic(receipt_path, receipt);
            journal["actions"].back()["state"] = "committed";
            write_json_atomic(journal_path, journal);
            results.push_back(receipt);
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
                fs::remove(
                    action.at("configuration_backup").get<std::string>(),
                    ignored);
                ignored.clear();
            }
        }
        fs::remove_all(staging_root / transaction_id, ignored);
    } catch (const std::exception& error) {
        std::error_code rollback_error;
        for (auto action = journal["actions"].rbegin();
             action != journal["actions"].rend();
             ++action) {
            const std::string state = action->value("state", "staging");
            if (action->contains("knowledge_activation")) {
                const auto& activation = action->at("knowledge_activation");
                const fs::path destination =
                    activation.at("path").get<std::string>();
                fs::remove_all(destination, rollback_error);
                rollback_error.clear();
                const std::string backup = activation.value("backup_path", "");
                if (!backup.empty() && fs::exists(backup)) {
                    fs::rename(backup, destination, rollback_error);
                    rollback_error.clear();
                }
            }
            if (state != "staging" && action->contains("target")) {
                fs::remove_all(
                    action->at("target").get<std::string>(), rollback_error);
                rollback_error.clear();
            }
            if (action->contains("rollback_path") &&
                action->contains("target")) {
                const fs::path rollback =
                    action->at("rollback_path").get<std::string>();
                const fs::path target = action->at("target").get<std::string>();
                if (fs::exists(rollback) && !fs::exists(target)) {
                    fs::rename(rollback, target, rollback_error);
                    rollback_error.clear();
                }
            }
            if (action->contains("receipt_path")) {
                const fs::path receipt =
                    action->at("receipt_path").get<std::string>();
                fs::remove(receipt, rollback_error);
                rollback_error.clear();
                if (action->contains("receipt_backup")) {
                    const fs::path backup =
                        action->at("receipt_backup").get<std::string>();
                    if (fs::exists(backup)) {
                        fs::rename(backup, receipt, rollback_error);
                        rollback_error.clear();
                    }
                }
            }
            if (action->contains("configuration_path")) {
                const fs::path configuration =
                    action->at("configuration_path").get<std::string>();
                fs::remove(configuration, rollback_error);
                rollback_error.clear();
                if (action->contains("configuration_backup")) {
                    const fs::path backup =
                        action->at("configuration_backup").get<std::string>();
                    if (fs::exists(backup)) {
                        fs::rename(
                            backup, configuration, rollback_error);
                        rollback_error.clear();
                    }
                }
            }
            (*action)["state"] = "rolled_back";
        }
        fs::remove_all(staging_root / transaction_id, rollback_error);
        journal["state"] = "rolled_back";
        journal["error"] = error.what();
        write_json_atomic(journal_path, journal);
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
            results.push_back(
                {{"component_id", receipt.value("component_id", "unknown")},
                 {"state", "broken"},
                 {"error", receipt.value("error", "receipt is incomplete")}});
            continue;
        }
        try {
            validate_component_payload(
                receipt.at("active_root").get<std::string>(),
                receipt.at("component_id").get<std::string>(),
                receipt.at("component_version").get<std::string>());
            for (const auto& activated :
                 receipt.value("activated_files", json::array())) {
                const fs::path path = activated.at("path").get<std::string>();
                if (!within(paths.knowledge_root, path) ||
                    !fs::is_regular_file(path) ||
                    fs::file_size(path) != integer_field(activated, "size_bytes") ||
                    sha256_file(path) !=
                        activated.at("sha256").get<std::string>()) {
                    throw std::runtime_error(
                        "activated knowledge file verification failed: " +
                        path.string());
                }
            }
            results.push_back(
                {{"component_id", receipt.at("component_id")}, {"state", "verified"}});
        } catch (const std::exception& error) {
            ok = false;
            results.push_back(
                {{"component_id", receipt.at("component_id")},
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

json SetupEngine::uninstall(
    const SetupPaths& paths,
    const std::vector<std::string>& component_ids,
    const bool confirmed,
    const bool purge) {
    if (!confirmed) {
        throw std::runtime_error("uninstall requires explicit confirmation");
    }
    json results = json::array();
    for (const auto& id : component_ids) {
        if (!valid_identifier(id)) {
            throw std::runtime_error("invalid component id " + id);
        }
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
        std::error_code error;
        fs::remove_all(active_root, error);
        if (error) {
            throw std::runtime_error("cannot remove managed component " + id);
        }
        if (purge) {
            for (const auto& data_path_value :
                 receipt.value("data_paths", json::array())) {
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
            for (const auto& activated :
                 receipt.value("activated_files", json::array())) {
                const fs::path activated_path =
                    activated.at("path").get<std::string>();
                if (!within(paths.knowledge_root, activated_path)) {
                    throw std::runtime_error(
                        "knowledge purge path is outside the managed knowledge root");
                }
                if (fs::is_regular_file(activated_path) &&
                    sha256_file(activated_path) ==
                        activated.at("sha256").get<std::string>()) {
                    fs::remove(activated_path, error);
                    if (error) {
                        throw std::runtime_error(
                            "cannot purge managed knowledge for " + id);
                    }
                }
            }
        }
        fs::remove(receipt_path);
        results.push_back(
            {{"component_id", id},
             {"state", "uninstalled"},
             {"data_preserved", !purge}});
    }
    return {
        {"schema", "arena_install_uninstall_result_v1"},
        {"components", std::move(results)},
    };
}

}  // namespace arena::application::installer
