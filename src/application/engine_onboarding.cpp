#include "arena/application/engine_onboarding.hpp"
#include "arena/adapters/uci_process.hpp"
#include "arena/platform/path_inspection.hpp"

#include <sodium.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <optional>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <system_error>

namespace arena::application {
namespace {

namespace fs = std::filesystem;
using nlohmann::json;

constexpr std::uintmax_t max_metadata_bytes = 1024 * 1024;
constexpr std::size_t max_receipts = 1024;
constexpr std::size_t max_diagnostic_path_characters = 512;
constexpr std::size_t max_engine_arguments = 32;
constexpr std::size_t max_engine_options = 128;

json read_json_bounded(const fs::path& path) {
    std::error_code error;
    const auto size = fs::file_size(path, error);
    if (error || size > max_metadata_bytes) {
        throw std::runtime_error("metadata file is unavailable or exceeds one MiB");
    }
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        throw std::runtime_error("metadata file cannot be opened");
    }
    json value;
    input >> value;
    return value;
}

fs::path normalized_absolute(const fs::path& value) {
    std::error_code error;
    fs::path result = fs::weakly_canonical(value, error);
    if (error) {
        result = fs::absolute(value, error);
    }
    return (error ? value : result).lexically_normal();
}

bool safe_relative_path(const fs::path& value) {
    if (value.empty() || value.is_absolute() || value.has_root_name()) {
        return false;
    }
    return std::none_of(
        value.begin(), value.end(),
        [](const fs::path& part) { return part == "." || part == ".."; });
}

bool within(const fs::path& root, const fs::path& candidate) {
    const fs::path relative = candidate.lexically_relative(root);
    return !relative.empty() && !relative.is_absolute() &&
           std::ranges::none_of(
               relative,
               [](const fs::path& part) { return part == ".."; });
}

fs::path lexical_absolute(const fs::path& value) {
    std::error_code error;
    const fs::path absolute = fs::absolute(value, error);
    return (error ? value : absolute).lexically_normal();
}

std::string sha256_file(const fs::path& path) {
    if (sodium_init() < 0) {
        throw std::runtime_error("libsodium initialization failed");
    }
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        throw std::runtime_error("installed component file cannot be verified");
    }
    crypto_hash_sha256_state state{};
    crypto_hash_sha256_init(&state);
    std::array<unsigned char, 64 * 1024> buffer{};
    while (input) {
        input.read(reinterpret_cast<char*>(buffer.data()), buffer.size());
        const auto count = input.gcount();
        if (count > 0) {
            crypto_hash_sha256_update(
                &state, buffer.data(),
                static_cast<unsigned long long>(count));
        }
    }
    std::array<unsigned char, crypto_hash_sha256_BYTES> digest{};
    crypto_hash_sha256_final(&state, digest.data());
    std::array<char, crypto_hash_sha256_BYTES * 2 + 1> encoded{};
    sodium_bin2hex(
        encoded.data(), encoded.size(), digest.data(), digest.size());
    return encoded.data();
}

std::optional<fs::path> discovery_path(
    const json& manifest,
    const std::string& kind) {
    if (!manifest.contains("discovery") ||
        !manifest.at("discovery").is_array() ||
        manifest.at("discovery").size() > 16U) {
        return std::nullopt;
    }
    std::optional<fs::path> result;
    for (const auto& declaration : manifest.at("discovery")) {
        if (!declaration.is_object() ||
            declaration.value("kind", "") != kind) {
            continue;
        }
        if (result || !declaration.contains("locator") ||
            !declaration.at("locator").is_object()) {
            return std::nullopt;
        }
        const json& locator = declaration.at("locator");
        const std::string source = locator.value("source", "");
        if (source == "entrypoint") {
            if (!locator.contains("entrypoint") ||
                !locator.at("entrypoint").is_string() ||
                !manifest.contains("entrypoints") ||
                !manifest.at("entrypoints").is_object()) {
                return std::nullopt;
            }
            const std::string name =
                locator.at("entrypoint").get<std::string>();
            const json& entrypoints = manifest.at("entrypoints");
            if (!entrypoints.contains(name) ||
                !entrypoints.at(name).is_string()) {
                return std::nullopt;
            }
            result = entrypoints.at(name).get<std::string>();
        } else if (source == "component_path") {
            if (!locator.contains("path") ||
                !locator.at("path").is_string()) {
                return std::nullopt;
            }
            result = locator.at("path").get<std::string>();
        } else {
            return std::nullopt;
        }
        if (!safe_relative_path(*result)) {
            return std::nullopt;
        }
    }
    return result;
}

bool verified_manifest_file(
    const fs::path& active_root,
    const json& manifest,
    const fs::path& selected_relative) {
    if (!safe_relative_path(selected_relative) ||
        !manifest.contains("files") || !manifest.at("files").is_array()) {
        return false;
    }
    const fs::path selected = selected_relative.lexically_normal();
    std::set<fs::path> listed;
    const json* selected_file = nullptr;
    for (const auto& file : manifest.at("files")) {
        if (!file.is_object() || !file.contains("path") ||
            !file.at("path").is_string()) {
            return false;
        }
        const fs::path relative =
            fs::path(file.at("path").get<std::string>()).lexically_normal();
        if (!safe_relative_path(relative) || !listed.insert(relative).second) {
            return false;
        }
        if (relative == selected) {
            selected_file = &file;
        }
    }
    if (selected_file == nullptr || !selected_file->contains("sha256") ||
        !selected_file->at("sha256").is_string() ||
        !selected_file->contains("size_bytes") ||
        !selected_file->at("size_bytes").is_number_unsigned()) {
        return false;
    }
    const fs::path executable = active_root / selected;
    std::error_code error;
    if (!fs::is_regular_file(executable, error) || error ||
        fs::is_symlink(executable, error) || error ||
        fs::file_size(executable, error) !=
            selected_file->at("size_bytes").get<std::uintmax_t>() ||
        error) {
        return false;
    }
    const std::string expected =
        selected_file->at("sha256").get<std::string>();
    return expected.size() == crypto_hash_sha256_BYTES * 2U &&
           sha256_file(executable) == expected;
}

bool usable_executable(const fs::path& value) {
    const auto observation = platform::inspect_path_no_follow(value);
    return observation.kind == platform::PathEntryKind::regular_file &&
           observation.executable;
}

bool bounded_uci_scalar(const json& value) {
    if (value.is_boolean() || value.is_number_integer() ||
        value.is_number_unsigned()) {
        return true;
    }
    if (!value.is_string()) {
        return false;
    }
    const std::string& text = value.get_ref<const std::string&>();
    return text.size() <= 4096U &&
           std::ranges::none_of(
               text,
               [](const unsigned char character) {
                   return character == '\0' || character == '\r' ||
                          character == '\n';
               });
}

bool declares_capability(const json& manifest, const std::string& expected) {
    const json capabilities = manifest.value("capabilities", json::array());
    return capabilities.is_array() && capabilities.size() <= 128U &&
           std::ranges::any_of(
               capabilities,
               [&expected](const json& capability) {
                   return capability.is_string() && capability == expected;
               });
}

bool bounded_identity_token(const json& value, const bool decimal_only = false) {
    if (!value.is_string()) {
        return false;
    }
    const std::string& text = value.get_ref<const std::string&>();
    return !text.empty() && text.size() <= 128U &&
           std::ranges::all_of(
               text,
               [decimal_only](const unsigned char character) {
                   return decimal_only
                       ? std::isdigit(character) != 0
                       : std::isalnum(character) != 0 || character == '_' ||
                             character == '-' || character == '.';
               });
}

json validated_managed_launch_profile(
    const json& receipt,
    const fs::path& active_root,
    const fs::path& executable) {
    if (!receipt.contains("configuration_path") ||
        !receipt.at("configuration_path").is_string() ||
        receipt.at("configuration_path").get_ref<const std::string&>().empty()) {
        return nullptr;
    }
    const auto data_paths = receipt.value("data_paths", json::array());
    if (!data_paths.is_array() || data_paths.empty() || data_paths.size() > 16U) {
        throw std::runtime_error(
            "managed launch profile has no bounded preserved data root");
    }
    const fs::path configuration = normalized_absolute(
        receipt.at("configuration_path").get<std::string>());
    bool bounded = false;
    for (const auto& value : data_paths) {
        if (!value.is_string()) {
            throw std::runtime_error(
                "managed launch profile data root is malformed");
        }
        const fs::path data_root = normalized_absolute(value.get<std::string>());
        if (within(data_root, configuration)) {
            bounded = true;
            break;
        }
    }
    std::error_code error;
    if (!bounded || !fs::is_regular_file(configuration, error) || error ||
        fs::is_symlink(configuration, error) || error) {
        throw std::runtime_error(
            "managed launch profile is unavailable outside its preserved data root");
    }
    const json profile = read_json_bounded(configuration);
    if (!profile.is_object() ||
        profile.value("schema", "") != "arena_uci_engine_launch_profile_v1" ||
        profile.value("schema_version", 0) != 1 ||
        !profile.contains("component") ||
        !profile.at("component").is_object() ||
        !profile.contains("engine") || !profile.at("engine").is_object() ||
        !profile.contains("state") || !profile.at("state").is_string() ||
        !profile.contains("enabled") || !profile.at("enabled").is_boolean() ||
        !profile.contains("uci_options") ||
        !profile.at("uci_options").is_object() ||
        profile.at("uci_options").size() > max_engine_options ||
        !profile.contains("expected_runtime") ||
        !(profile.at("expected_runtime").is_null() ||
          profile.at("expected_runtime").is_object()) ||
        !profile.contains("evidence") || !profile.at("evidence").is_object() ||
        !profile.contains("knowledge") || !profile.at("knowledge").is_object() ||
        !profile.contains("diagnostics") ||
        !profile.at("diagnostics").is_array() ||
        profile.at("diagnostics").size() > 32U) {
        throw std::runtime_error("managed launch profile contract is invalid");
    }
    const std::string state = profile.at("state").get<std::string>();
    if ((state != "conservative" && state != "qualified_timing") ||
        !profile.at("enabled").get<bool>()) {
        throw std::runtime_error("managed launch profile state is unavailable");
    }
    const json& expected_runtime = profile.at("expected_runtime");
    if (state == "conservative" && !expected_runtime.is_null()) {
        throw std::runtime_error(
            "conservative launch profile invents qualified runtime identity");
    }
    if (state == "qualified_timing") {
        static constexpr std::array<const char*, 7> token_fields{
            "timing_contract_version",
            "model_id",
            "adapter_id",
            "provider_requested",
            "provider_effective",
            "onnx_runtime_version",
            "search_core",
        };
        static constexpr std::array<const char*, 3> fingerprint_fields{
            "model_fingerprint",
            "provider_numeric_fingerprint",
            "hardware_driver_fingerprint",
        };
        if (!expected_runtime.is_object() ||
            std::ranges::any_of(
                token_fields,
                [&expected_runtime](const char* field) {
                    return !expected_runtime.contains(field) ||
                           !bounded_identity_token(expected_runtime.at(field));
                }) ||
            std::ranges::any_of(
                fingerprint_fields,
                [&expected_runtime](const char* field) {
                    return !expected_runtime.contains(field) ||
                           !bounded_identity_token(
                               expected_runtime.at(field), true);
                })) {
            throw std::runtime_error(
                "qualified launch profile runtime identity is incomplete");
        }
    }
    const json& component = profile.at("component");
    const json& engine = profile.at("engine");
    if (component.value("id", "") != receipt.value("component_id", "") ||
        component.value("version", "") !=
            receipt.value("component_version", "") ||
        !component.contains("root") || !component.at("root").is_string() ||
        normalized_absolute(component.at("root").get<std::string>()) != active_root ||
        !engine.contains("executable") ||
        !engine.at("executable").is_string() ||
        normalized_absolute(engine.at("executable").get<std::string>()) != executable ||
        !engine.contains("working_directory") ||
        !engine.at("working_directory").is_string() ||
        normalized_absolute(engine.at("working_directory").get<std::string>()) !=
            active_root ||
        !engine.contains("arguments") || !engine.at("arguments").is_array() ||
        engine.at("arguments").size() > max_engine_arguments) {
        throw std::runtime_error(
            "managed launch profile does not bind the installed component identity");
    }
    for (const auto& argument : engine.at("arguments")) {
        if (!argument.is_string() ||
            !bounded_uci_scalar(argument)) {
            throw std::runtime_error("managed launch argument is invalid");
        }
    }
    for (const auto& [name, value] : profile.at("uci_options").items()) {
        if (name.empty() || name.size() > 128U ||
            !std::ranges::all_of(
                name,
                [](const unsigned char character) {
                    return std::isalnum(character) != 0 || character == ' ' ||
                           character == '_' || character == '-';
                }) ||
            !bounded_uci_scalar(value)) {
            throw std::runtime_error("managed launch UCI option is invalid");
        }
    }
    return profile;
}

std::string path_key(const fs::path& value) {
    std::string key = lexical_absolute(value).generic_string();
#ifdef _WIN32
    std::ranges::transform(
        key, key.begin(),
        [](const unsigned char character) {
            return static_cast<char>(std::tolower(character));
        });
#endif
    return key;
}

std::string candidate_id(const fs::path& executable) {
    std::uint64_t hash = 1469598103934665603ULL;
    for (const unsigned char character : path_key(executable)) {
        hash ^= character;
        hash *= 1099511628211ULL;
    }
    std::ostringstream output;
    output << "engcand-" << std::hex << std::setfill('0')
           << std::setw(16) << hash;
    return output.str();
}

std::string display_name_for(const fs::path& executable) {
    std::string value = executable.stem().string();
    std::replace(value.begin(), value.end(), '_', ' ');
    std::replace(value.begin(), value.end(), '-', ' ');
    return value.empty() ? "UCI engine" : value;
}

void add_candidate(
    json& candidates,
    std::set<std::string>& seen,
    const std::string& source,
    const fs::path& executable,
    const fs::path& working_directory,
    const std::string& display_name,
    const json& metadata = json::object(),
    const std::optional<bool> observed_usable = std::nullopt) {
    const fs::path normalized_executable = observed_usable.has_value()
        ? lexical_absolute(executable)
        : normalized_absolute(executable);
    if (!seen.insert(path_key(normalized_executable)).second) {
        return;
    }
    json issues = json::array();
    const bool usable = observed_usable.value_or(
        usable_executable(normalized_executable));
    if (!usable) {
        issues.push_back({
            {"code", "engine_executable_unavailable"},
            {"detail",
             "The Manager service cannot access a regular executable at this path."},
        });
    }
    json candidate{
        {"schema", "arena_engine_candidate_v1"},
        {"schema_version", 1},
        {"candidate_id", candidate_id(normalized_executable)},
        {"source", source},
        {"state", usable ? "ready_to_probe" : "unavailable"},
        {"display_name",
         display_name.empty() ? display_name_for(normalized_executable) :
                                display_name},
        {"executable", normalized_executable.string()},
        {"working_directory",
         (observed_usable.has_value()
              ? lexical_absolute(working_directory)
              : normalized_absolute(working_directory))
             .string()},
        {"arguments", json::array()},
        {"uci_options", json::object()},
        {"authority", "discovery_hint"},
        {"issues", std::move(issues)},
    };
    for (const auto& [key, value] : metadata.items()) {
        candidate[key] = value;
    }
    if (candidate.value("recommended", false)) {
        candidates.insert(candidates.begin(), std::move(candidate));
    } else {
        candidates.push_back(std::move(candidate));
    }
}

void append_admitted_component_candidate(
    const json& receipt,
    const json& manifest,
    const fs::path& active_root,
    const fs::path& executable,
    json& candidates,
    std::set<std::string>& seen) {
    const fs::path canonical_root = normalized_absolute(active_root);
    const fs::path canonical_executable = normalized_absolute(executable);
    const std::string component_id =
        receipt.at("component_id").get<std::string>();
    json metadata{
        {"authority", "verified_component_inventory"},
        {"component_id", component_id},
        {"component_version", receipt.value("component_version", "")},
        {"recommended", component_id == "uci_arena.engine"},
    };
    const std::string artifact_sha256 =
        receipt.value("artifact_sha256", "");
    if (artifact_sha256.size() == crypto_hash_sha256_BYTES * 2U &&
        std::ranges::all_of(
            artifact_sha256,
            [](const unsigned char character) {
                return std::isxdigit(character) != 0;
            })) {
        metadata["installed_artifact_sha256"] = artifact_sha256;
    }
    if (manifest.contains("default_model") &&
        manifest.at("default_model").is_object()) {
        const json& model = manifest.at("default_model");
        if (model.contains("model_id") &&
            model.at("model_id").is_string() &&
            model.contains("display_name") &&
            model.at("display_name").is_string() &&
            model.contains("input_adapter_id") &&
            model.at("input_adapter_id").is_string() &&
            model.contains("root") && model.at("root").is_string()) {
            metadata["default_model"] = {
                {"model_id", model.at("model_id")},
                {"display_name", model.at("display_name")},
                {"input_adapter_id", model.at("input_adapter_id")},
                {"root", model.at("root")},
            };
            const fs::path relative_model_root =
                model.at("root").get<std::string>();
            if (safe_relative_path(relative_model_root)) {
                const fs::path model_root = normalized_absolute(
                    canonical_root / relative_model_root);
                if (platform::inspect_path_no_follow(model_root).kind ==
                    platform::PathEntryKind::directory) {
                    metadata["uci_options"] = {
                        {"ModelRoot", model_root.string()},
                    };
                }
            }
        }
    }
    if (declares_capability(manifest, "uci_engine_launch_profile_v1")) {
        try {
            const json launch_profile = validated_managed_launch_profile(
                receipt,
                canonical_root,
                canonical_executable);
            if (launch_profile.is_null()) {
                throw std::runtime_error(
                    "managed launch profile is not configured");
            }
            metadata["arguments"] =
                launch_profile.at("engine").at("arguments");
            metadata["uci_options"] = launch_profile.at("uci_options");
            json timing{{"available", false}};
            const json& evidence = launch_profile.at("evidence");
            if (evidence.contains("timing_profile") &&
                evidence.at("timing_profile").is_object()) {
                const json& profile = evidence.at("timing_profile");
                timing = {
                    {"available", true},
                    {"schema", profile.value("schema", "")},
                    {"identity_sha256",
                     profile.value("identity_sha256", "")},
                };
            }
            metadata["launch_profile"] = {
                {"schema", "arena_uci_engine_launch_profile_v1"},
                {"state", launch_profile.at("state")},
                {"timing_profile", std::move(timing)},
                {"expected_runtime", launch_profile.at("expected_runtime")},
            };
        } catch (const std::exception&) {
            metadata["state"] = "unavailable";
            metadata["uci_options"] = json::object();
            metadata["issues"] = json::array({
                {
                    {"code", "engine_launch_profile_invalid"},
                    {"detail",
                     "The managed launch profile does not match this verified "
                     "Engine installation. Repair the suite before registering "
                     "it."},
                },
            });
        }
    }
    add_candidate(
        candidates,
        seen,
        "install_receipt",
        canonical_executable,
        canonical_root,
        component_id == "uci_arena.engine"
            ? "UCI Arena Engine"
            : manifest.value("display_name", display_name_for(executable)),
        metadata);
}

void append_receipt_candidates(
    const fs::path& data_root,
    json& candidates,
    std::set<std::string>& seen) {
    const fs::path receipts_root = data_root / "receipts";
    std::error_code error;
    if (!fs::is_directory(receipts_root, error) ||
        fs::is_symlink(receipts_root, error)) {
        return;
    }
    std::vector<fs::path> receipts;
    for (const auto& entry : fs::directory_iterator(
             receipts_root, fs::directory_options::skip_permission_denied,
             error)) {
        if (receipts.size() >= max_receipts) {
            break;
        }
        if (entry.is_regular_file(error) && !entry.is_symlink(error) &&
            entry.path().extension() == ".json") {
            receipts.push_back(entry.path());
        }
    }
    std::ranges::sort(receipts);
    for (const auto& receipt_path : receipts) {
        try {
            const json receipt = read_json_bounded(receipt_path);
            if (receipt.value("schema", "") != "arena_install_receipt_v1" ||
                receipt.value("schema_version", 0) != 1 ||
                receipt.value("state", "") != "installed_managed" ||
                !receipt.contains("component_id") ||
                !receipt.at("component_id").is_string() ||
                !receipt.contains("component_version") ||
                !receipt.at("component_version").is_string() ||
                !receipt.contains("active_root") ||
                !receipt.at("active_root").is_string()) {
                continue;
            }
            const std::string component_id =
                receipt.at("component_id").get<std::string>();
            if (receipt_path.filename() != component_id + ".json") {
                continue;
            }
            const fs::path declared_root =
                receipt.at("active_root").get<std::string>();
            if (fs::is_symlink(declared_root, error) || error) {
                error.clear();
                continue;
            }
            const fs::path active_root = normalized_absolute(declared_root);
            const fs::path manifest_path = active_root / "arena-component.json";
            if (!fs::is_regular_file(manifest_path, error) || error ||
                fs::is_symlink(manifest_path, error) || error) {
                error.clear();
                continue;
            }
            const json manifest =
                read_json_bounded(manifest_path);
            if (manifest.value("schema", "") !=
                    "arena_install_component_v1" ||
                manifest.value("schema_version", 0) != 1 ||
                manifest.value("component_id", "") !=
                    component_id ||
                manifest.value("component_version", "") !=
                    receipt.value("component_version", "")) {
                continue;
            }
            const auto relative = discovery_path(manifest, "uci_engine");
            if (!relative ||
                !verified_manifest_file(active_root, manifest, *relative)) {
                continue;
            }
            const fs::path executable = active_root / *relative;
            append_admitted_component_candidate(
                receipt,
                manifest,
                active_root,
                executable,
                candidates,
                seen);
        } catch (const std::exception&) {
        }
    }
}

void append_registry_candidates(
    const ComponentVerificationRegistry& component_registry,
    json& candidates,
    std::set<std::string>& seen) {
    for (const auto& component : component_registry.verified_components()) {
        try {
            const json& receipt = component.receipt;
            const json& manifest = component.manifest;
            if (receipt.value("state", "") != "installed_managed") {
                continue;
            }
            const auto relative = discovery_path(manifest, "uci_engine");
            if (!relative || !manifest.contains("files") ||
                !manifest.at("files").is_array()) {
                continue;
            }
            const fs::path normalized_relative = relative->lexically_normal();
            const auto file = std::ranges::find_if(
                manifest.at("files"),
                [&normalized_relative](const json& value) {
                    return value.is_object() && value.contains("path") &&
                           value.at("path").is_string() &&
                           fs::path(value.at("path").get<std::string>())
                                   .lexically_normal() == normalized_relative;
                });
            if (file == manifest.at("files").end()) {
                continue;
            }
            const fs::path executable =
                component.active_root / normalized_relative;
            const auto observation =
                platform::inspect_path_no_follow(executable);
            if (observation.kind != platform::PathEntryKind::regular_file ||
                observation.size_bytes != file->value("size_bytes", 0ULL)) {
                continue;
            }
            append_admitted_component_candidate(
                receipt,
                manifest,
                component.active_root,
                executable,
                candidates,
                seen);
        } catch (const std::exception&) {
        }
    }
}

void append_saved_locator(
    const fs::path& data_root,
    json& candidates,
    std::set<std::string>& seen) {
    const fs::path locator_path =
        data_root / "locators" / "uci_engine.json";
    std::error_code error;
    if (!fs::is_regular_file(locator_path, error) ||
        fs::is_symlink(locator_path, error)) {
        return;
    }
    try {
        const json locator = read_json_bounded(locator_path);
        if (locator.value("schema", "") != "arena_saved_locator_v1" ||
            locator.value("schema_version", 0) != 1 ||
            locator.value("kind", "") != "uci_engine" ||
            !locator.contains("path")) {
            return;
        }
        const fs::path executable =
            normalized_absolute(locator.at("path").get<std::string>());
        const json validation =
            locator.value("validation", json::object());
        json metadata{{"authority", "retained_locator_hint"}};
        if (validation.contains("fingerprint") &&
            validation.at("fingerprint").is_string()) {
            metadata["fingerprint"] = validation.at("fingerprint");
        }
        add_candidate(
            candidates, seen, "saved_locator", executable,
            executable.parent_path(),
            validation.value("engine_name", display_name_for(executable)),
            metadata);
    } catch (const std::exception&) {
    }
}

class ExplicitDiscoveryWork final {
  public:
    explicit ExplicitDiscoveryWork(const EngineDiscoveryOptions& requested)
        : limits_(bounded(requested)), started_(Clock::now()) {}

    [[nodiscard]] const EngineDiscoveryOptions& limits() const noexcept {
        return limits_;
    }

    [[nodiscard]] bool active() {
        if (halted_) {
            return false;
        }
        if (limits_.cancellation.stop_requested()) {
            cancelled_ = true;
            fail("discovery_cancelled", {});
            return false;
        }
        if (Clock::now() - started_ >= limits_.max_elapsed) {
            fail("discovery_deadline_exceeded", {});
            return false;
        }
        return true;
    }

    [[nodiscard]] bool consume_entry(const fs::path& path) {
        if (!active()) {
            return false;
        }
        ++entries_;
        if (entries_ > limits_.max_total_entries) {
            fail("entry_budget_exceeded", path);
            return false;
        }
        return true;
    }

    [[nodiscard]] std::optional<platform::PathObservation> inspect(
        const fs::path& path) {
        if (!active()) {
            return std::nullopt;
        }
        ++metadata_calls_;
        if (metadata_calls_ > limits_.max_metadata_calls) {
            fail("metadata_budget_exceeded", path);
            return std::nullopt;
        }
        const auto observation = platform::inspect_path_no_follow(path);
        if (observation.kind == platform::PathEntryKind::missing) {
            fail("entry_disappeared", path, observation.platform_error);
            return std::nullopt;
        }
        if (observation.kind == platform::PathEntryKind::inaccessible) {
            fail("entry_inaccessible", path, observation.platform_error);
            return std::nullopt;
        }
        return observation;
    }

    [[nodiscard]] bool reserve_candidate(const fs::path& path) {
        if (!active()) {
            return false;
        }
        ++candidates_;
        if (candidates_ > limits_.max_candidates) {
            fail("candidate_budget_exceeded", path);
            return false;
        }
        return true;
    }

    void fail(
        const std::string& code,
        const fs::path& path,
        const std::uint32_t platform_error = 0) {
        complete_ = false;
        halted_ = code == "discovery_cancelled" ||
                  code == "discovery_deadline_exceeded" ||
                  code == "entry_budget_exceeded" ||
                  code == "metadata_budget_exceeded" ||
                  code == "candidate_budget_exceeded" ||
                  code == "explicit_root_budget_exceeded";
        if (diagnostics_.size() >= limits_.max_diagnostics) {
            return;
        }
        std::string rendered = path.generic_string();
        rendered.resize(std::min(rendered.size(), max_diagnostic_path_characters));
        json diagnostic{{"code", code}};
        if (!rendered.empty()) {
            diagnostic["path"] = rendered;
        }
        if (platform_error != 0) {
            diagnostic["platform_error"] = platform_error;
        }
        diagnostics_.push_back(std::move(diagnostic));
    }

    [[nodiscard]] json projection() const {
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            Clock::now() - started_);
        return {
            {"authority", "discovery_hint"},
            {"complete", complete_},
            {"cancelled", cancelled_},
            {"work",
             {{"entries", entries_},
              {"metadata_calls", metadata_calls_},
              {"candidates", candidates_},
              {"elapsed_ms", elapsed.count()}}},
            {"limits",
             {{"max_roots", limits_.max_explicit_roots},
              {"max_depth_below_root", limits_.max_depth_below_root},
              {"max_total_entries", limits_.max_total_entries},
              {"max_metadata_calls", limits_.max_metadata_calls},
              {"max_candidates", limits_.max_candidates},
              {"max_elapsed_ms", limits_.max_elapsed.count()},
              {"max_diagnostics", limits_.max_diagnostics}}},
            {"diagnostics", diagnostics_},
        };
    }

  private:
    using Clock = std::chrono::steady_clock;

    static EngineDiscoveryOptions bounded(
        const EngineDiscoveryOptions& requested) {
        const EngineDiscoveryOptions maximum{};
        EngineDiscoveryOptions result = requested;
        result.max_explicit_roots =
            std::min(result.max_explicit_roots, maximum.max_explicit_roots);
        result.max_depth_below_root =
            std::min(result.max_depth_below_root, maximum.max_depth_below_root);
        result.max_total_entries =
            std::min(result.max_total_entries, maximum.max_total_entries);
        result.max_metadata_calls =
            std::min(result.max_metadata_calls, maximum.max_metadata_calls);
        result.max_candidates =
            std::min(result.max_candidates, maximum.max_candidates);
        result.max_elapsed = std::clamp(
            result.max_elapsed, std::chrono::milliseconds{0},
            maximum.max_elapsed);
        result.max_diagnostics =
            std::min(result.max_diagnostics, maximum.max_diagnostics);
        return result;
    }

    EngineDiscoveryOptions limits_;
    Clock::time_point started_;
    std::size_t entries_{0};
    std::size_t metadata_calls_{0};
    std::size_t candidates_{0};
    bool complete_{true};
    bool cancelled_{false};
    bool halted_{false};
    json diagnostics_ = json::array();
};

void append_observed_candidate(
    const fs::path& path,
    const platform::PathObservation& observation,
    json& candidates,
    std::set<std::string>& seen,
    ExplicitDiscoveryWork& work) {
    if (!observation.executable || !work.reserve_candidate(path)) {
        return;
    }
    add_candidate(
        candidates, seen, "discovered", path, path.parent_path(),
        display_name_for(path),
        {
            {"authority", "discovery_hint"},
            {"observation",
             {{"object_identity", observation.object_identity},
              {"size_bytes", observation.size_bytes},
              {"path_kind", "regular_file"}}},
        },
        true);
}

void scan_explicit_directory(
    const fs::path& directory,
    const std::size_t depth,
    json& candidates,
    std::set<std::string>& seen,
    ExplicitDiscoveryWork& work) {
    if (!work.active()) {
        return;
    }
    std::error_code error;
    fs::directory_iterator iterator(directory, fs::directory_options::none, error);
    if (error) {
        work.fail("directory_iterator_open_failed", directory, error.value());
        return;
    }
    std::vector<fs::path> entries;
    const fs::directory_iterator end;
    while (iterator != end) {
        const fs::path entry_path = iterator->path();
        if (!work.consume_entry(entry_path)) {
            return;
        }
        entries.push_back(entry_path);
        iterator.increment(error);
        if (error) {
            work.fail("directory_iterator_increment_failed", directory, error.value());
            return;
        }
    }
    std::ranges::sort(entries, [](const fs::path& left, const fs::path& right) {
        return left.generic_string() < right.generic_string();
    });
    for (const auto& entry_path : entries) {
        const auto observation = work.inspect(entry_path);
        if (!observation) {
            if (!work.active()) {
                return;
            }
            continue;
        }
        if (observation->kind == platform::PathEntryKind::regular_file) {
            append_observed_candidate(
                entry_path, *observation, candidates, seen, work);
        } else if (
            observation->kind == platform::PathEntryKind::directory &&
            depth < work.limits().max_depth_below_root) {
            scan_explicit_directory(
                entry_path, depth + 1, candidates, seen, work);
        }
        if (!work.active()) {
            return;
        }
    }
}

void append_explicit_root(
    const fs::path& supplied_root,
    json& candidates,
    std::set<std::string>& seen,
    ExplicitDiscoveryWork& work) {
    const fs::path root = lexical_absolute(supplied_root);
    const auto observation = work.inspect(root);
    if (!observation) {
        return;
    }
    if (observation->kind == platform::PathEntryKind::regular_file) {
        append_observed_candidate(root, *observation, candidates, seen, work);
    } else if (observation->kind == platform::PathEntryKind::directory) {
        scan_explicit_directory(root, 0, candidates, seen, work);
    }
}

void append_path_candidates(
    json& candidates,
    std::set<std::string>& seen) {
    const char* path_environment = std::getenv("PATH");
    if (path_environment == nullptr) {
        return;
    }
#ifdef _WIN32
    constexpr char separator = ';';
    constexpr std::array names{
        "uci_arena_engine.exe", "stockfish.exe", "lc0.exe"};
#else
    constexpr char separator = ':';
    constexpr std::array names{"uci_arena_engine", "stockfish", "lc0"};
#endif
    std::stringstream paths(path_environment);
    for (std::string item; std::getline(paths, item, separator);) {
        if (item.empty()) {
            continue;
        }
        for (const auto* name : names) {
            const fs::path executable = fs::path(item) / name;
            if (usable_executable(executable)) {
                add_candidate(
                    candidates, seen, "discovered", executable,
                    executable.parent_path(), display_name_for(executable));
            }
        }
    }
}

}  // namespace

static json discover_impl(
    const fs::path& manager_data_root,
    const ComponentVerificationRegistry* component_registry,
    const std::vector<fs::path>& explicit_roots,
    const bool include_path_candidates,
    const EngineDiscoveryOptions& options) {
    json candidates = json::array();
    std::set<std::string> seen;
    if (component_registry != nullptr) {
        append_registry_candidates(*component_registry, candidates, seen);
    } else {
        append_receipt_candidates(manager_data_root, candidates, seen);
    }
    append_saved_locator(manager_data_root, candidates, seen);
    ExplicitDiscoveryWork explicit_work(options);
    if (explicit_roots.size() >
        explicit_work.limits().max_explicit_roots) {
        explicit_work.fail("explicit_root_budget_exceeded", {});
    } else {
        std::vector<fs::path> ordered_roots = explicit_roots;
        std::ranges::sort(
            ordered_roots,
            [](const fs::path& left, const fs::path& right) {
                return left.generic_string() < right.generic_string();
            });
        for (const auto& root : ordered_roots) {
            append_explicit_root(root, candidates, seen, explicit_work);
            if (!explicit_work.active()) {
                break;
            }
        }
    }
    if (include_path_candidates) {
        append_path_candidates(candidates, seen);
    }
    return {
        {"schema", "arena_engine_candidates_v1"},
        {"schema_version", 1},
        {"precedence",
         {"install_receipt", "saved_locator", "provider_manifest",
          "discovered"}},
        {"explicit_root_inspection", explicit_work.projection()},
        {"candidates", std::move(candidates)},
    };
}

json EngineOnboarding::discover(
    const fs::path& manager_data_root,
    const std::vector<fs::path>& explicit_roots,
    const bool include_path_candidates,
    const EngineDiscoveryOptions& options) {
    return discover_impl(
        manager_data_root,
        nullptr,
        explicit_roots,
        include_path_candidates,
        options);
}

json EngineOnboarding::discover(
    const fs::path& manager_data_root,
    const ComponentVerificationRegistry& component_registry,
    const std::vector<fs::path>& explicit_roots,
    const bool include_path_candidates,
    const EngineDiscoveryOptions& options) {
    return discover_impl(
        manager_data_root,
        &component_registry,
        explicit_roots,
        include_path_candidates,
        options);
}

std::optional<std::string> EngineOnboarding::runtime_identity_mismatch(
    const json& expected,
    const adapters::UciRuntimeStatus& observed) {
    const std::array<std::pair<const char*, const std::string*>, 10> fields{{
        {"timing_contract_version", &observed.timing_contract_version},
        {"model_id", &observed.model_id},
        {"model_fingerprint", &observed.model_fingerprint},
        {"adapter_id", &observed.adapter_id},
        {"provider_requested", &observed.provider_requested},
        {"provider_effective", &observed.provider_effective},
        {"provider_numeric_fingerprint", &observed.provider_numeric_fingerprint},
        {"hardware_driver_fingerprint", &observed.hardware_driver_fingerprint},
        {"onnx_runtime_version", &observed.onnx_runtime_version},
        {"search_core", &observed.search_core},
    }};
    for (const auto& [field, actual] : fields) {
        if (!expected.is_object() ||
            expected.value(field, std::string{}) != *actual) {
            return field;
        }
    }
    return std::nullopt;
}

}  // namespace arena::application
