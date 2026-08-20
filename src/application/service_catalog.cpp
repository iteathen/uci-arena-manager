#include "arena/application/service_catalog.hpp"

#include <sodium.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <set>
#include <stdexcept>
#include <string_view>
#include <tuple>
#include <vector>

namespace arena::application {
namespace {

using nlohmann::json;
namespace fs = std::filesystem;

json read_json_file(const fs::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        throw std::runtime_error("cannot read installed component metadata");
    }
    return json::parse(input);
}

std::optional<fs::path> saved_locator_reference(
    const fs::path& manager_data_root,
    const std::string& kind,
    const bool allow_directory = false) {
    const fs::path metadata =
        manager_data_root / "locators" / (kind + ".json");
    if (!fs::is_regular_file(metadata) || fs::is_symlink(metadata) ||
        fs::file_size(metadata) > 1024ULL * 1024ULL) {
        return std::nullopt;
    }
    const json locator = read_json_file(metadata);
    if (locator.value("schema", "") != "arena_saved_locator_v1" ||
        locator.value("schema_version", 0) != 1 ||
        locator.value("kind", "") != kind ||
        !locator.contains("path") || !locator.at("path").is_string()) {
        return std::nullopt;
    }
    const fs::path target =
        fs::absolute(locator.at("path").get<std::string>())
            .lexically_normal();
    if ((!fs::is_regular_file(target) &&
         !(allow_directory && fs::is_directory(target))) ||
        fs::is_symlink(target)) {
        return std::nullopt;
    }
    return target;
}

void project_bot_prerequisite(
    json& readiness,
    json& issues,
    const std::string& dimension,
    const bool ready,
    const std::string& issue_code,
    const std::string& detail) {
    readiness[dimension] = ready ? "ready" : "missing";
    if (!ready) {
        issues.push_back({
            {"code", issue_code},
            {"detail", detail},
        });
    }
}

std::set<std::string> retained_catalog_components(
    const fs::path& manager_data_root) {
    std::set<std::string> result;
    const fs::path catalog_path =
        manager_data_root / "release" / "catalog.json";
    if (!fs::is_regular_file(catalog_path) ||
        fs::is_symlink(catalog_path) ||
        fs::file_size(catalog_path) > 16ULL * 1024ULL * 1024ULL) {
        return result;
    }
    try {
        const json catalog = read_json_file(catalog_path);
        if (catalog.value("schema", "") != "arena_install_catalog_v1" ||
            !catalog.contains("components") ||
            !catalog.at("components").is_array()) {
            return result;
        }
        for (const auto& component : catalog.at("components")) {
            const std::string id = component.value("component_id", "");
            if (!id.empty()) {
                result.insert(id);
            }
        }
    } catch (const std::exception&) {
    }
    return result;
}

bool safe_relative_path(const fs::path& path) {
    if (path.empty() || path.is_absolute() || path.has_root_name()) {
        return false;
    }
    for (const auto& part : path) {
        if (part == "." || part == "..") {
            return false;
        }
    }
    return true;
}

bool valid_contract_identifier(const std::string_view value) {
    return !value.empty() && value.size() <= 128U &&
           std::ranges::all_of(
               value,
               [](const unsigned char character) {
                   return std::isalnum(character) != 0 || character == '.' ||
                          character == '_' || character == '-';
               });
}

bool within(const fs::path& root, const fs::path& candidate) {
    const fs::path normalized_root =
        fs::absolute(root).lexically_normal();
    const fs::path normalized_candidate =
        fs::absolute(candidate).lexically_normal();
    auto root_part = normalized_root.begin();
    auto candidate_part = normalized_candidate.begin();
    for (; root_part != normalized_root.end();
         ++root_part, ++candidate_part) {
        if (candidate_part == normalized_candidate.end() ||
            *candidate_part != *root_part) {
            return false;
        }
    }
    return normalized_candidate != normalized_root;
}

bool same_or_within(const fs::path& root, const fs::path& candidate) {
    return fs::absolute(root).lexically_normal() ==
               fs::absolute(candidate).lexically_normal() ||
           within(root, candidate);
}

bool valid_lichess_account_name(const std::string& value) {
    return !value.empty() && value.size() <= 64 &&
           std::ranges::all_of(
               value,
               [](const unsigned char character) {
                   return std::isalnum(character) != 0 ||
                          character == '_' || character == '-';
               });
}

std::string sha256_file(const fs::path& path) {
    if (sodium_init() < 0) {
        throw std::runtime_error("libsodium initialization failed");
    }
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        throw std::runtime_error("cannot verify installed entrypoint");
    }
    crypto_hash_sha256_state state{};
    crypto_hash_sha256_init(&state);
    std::array<unsigned char, 64 * 1024> buffer{};
    while (input) {
        input.read(reinterpret_cast<char*>(buffer.data()), buffer.size());
        const auto count = input.gcount();
        if (count > 0) {
            crypto_hash_sha256_update(
                &state,
                buffer.data(),
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

struct InstalledComponent {
    json receipt;
    json manifest;
    fs::path active_root;
};

void verify_component_files(
    const fs::path& active_root,
    const json& manifest,
    const std::string& component_id) {
    if (!manifest.contains("files") || !manifest.at("files").is_array()) {
        throw std::runtime_error(
            component_id + " component manifest has no file inventory");
    }
    std::set<fs::path> listed;
    for (const auto& file : manifest.at("files")) {
        if (!file.is_object() || !file.contains("path") ||
            !file.at("path").is_string()) {
            throw std::runtime_error(
                component_id + " component file inventory is malformed");
        }
        const fs::path relative = file.at("path").get<std::string>();
        if (!safe_relative_path(relative) ||
            !listed.insert(relative.lexically_normal()).second) {
            throw std::runtime_error(
                component_id + " component file inventory is unsafe");
        }
        const fs::path path = active_root / relative;
        if (!fs::is_regular_file(path) || fs::is_symlink(path) ||
            fs::file_size(path) != file.value("size_bytes", 0ULL) ||
            sha256_file(path) != file.value("sha256", "")) {
            throw std::runtime_error(
                component_id + " component file digest mismatch: " +
                relative.generic_string());
        }
    }
    for (const auto& entry : fs::recursive_directory_iterator(active_root)) {
        if (entry.is_symlink()) {
            throw std::runtime_error(
                component_id + " component contains a symbolic link");
        }
        if (!entry.is_regular_file()) {
            continue;
        }
        const fs::path relative =
            fs::relative(entry.path(), active_root).lexically_normal();
        if (relative != fs::path("arena-component.json") &&
            !listed.contains(relative)) {
            throw std::runtime_error(
                component_id + " component contains an unlisted file: " +
                relative.generic_string());
        }
    }
}

InstalledComponent installed_component(
    const fs::path& receipts_root,
    const std::string& component_id,
    const bool allow_disabled = false) {
    const json receipt =
        read_json_file(receipts_root / (component_id + ".json"));
    const std::string state = receipt.value("state", "");
    if (receipt.value("schema", "") != "arena_install_receipt_v1" ||
        receipt.value("component_id", "") != component_id ||
        (state != "installed_managed" &&
         !(allow_disabled && state == "installed_disabled"))) {
        throw std::runtime_error(
            component_id +
            (allow_disabled
                 ? " does not have a managed installation receipt"
                 : " does not have an enabled managed receipt"));
    }
    const fs::path active_root =
        receipt.at("active_root").get<std::string>();
    const json manifest = read_json_file(active_root / "arena-component.json");
    if (manifest.value("schema", "") != "arena_install_component_v1" ||
        manifest.value("component_id", "") != component_id ||
        manifest.value("component_version", "") !=
            receipt.value("component_version", "")) {
        throw std::runtime_error(
            component_id + " receipt and component manifest disagree");
    }
    verify_component_files(active_root, manifest, component_id);
    return {receipt, manifest, active_root};
}

fs::path verified_entrypoint(
    const InstalledComponent& component,
    const std::string& name) {
    const std::string component_id =
        component.receipt.at("component_id").get<std::string>();
    if (!component.manifest.contains("entrypoints") ||
        !component.manifest.at("entrypoints").is_object()) {
        throw std::runtime_error(
            component_id + " does not declare the " + name + " entrypoint");
    }
    const json& entrypoints = component.manifest.at("entrypoints");
    std::string resolved_name = name;
    if ((!entrypoints.contains(resolved_name) ||
         !entrypoints.at(resolved_name).is_string()) &&
        component_id == "node_runtime.private" && name == "node" &&
        entrypoints.contains("executable") &&
        entrypoints.at("executable").is_string()) {
        resolved_name = "executable";
    }
    if (!entrypoints.contains(resolved_name) ||
        !entrypoints.at(resolved_name).is_string()) {
        throw std::runtime_error(
            component_id + " does not declare the " + name + " entrypoint");
    }
    const fs::path relative =
        entrypoints.at(resolved_name).get<std::string>();
    if (!safe_relative_path(relative)) {
        throw std::runtime_error("installed component entrypoint is unsafe");
    }
    const fs::path path = component.active_root / relative;
    for (const auto& file : component.manifest.at("files")) {
        if (file.value("path", "") == relative.generic_string()) {
            if (!fs::is_regular_file(path) ||
                fs::file_size(path) != file.value("size_bytes", 0ULL) ||
                sha256_file(path) != file.value("sha256", "")) {
                throw std::runtime_error(
                    "installed component entrypoint digest mismatch");
            }
            return path;
        }
    }
    throw std::runtime_error(
        "installed component entrypoint is not in its signed file manifest");
}

fs::path verified_component_file(
    const InstalledComponent& component,
    const fs::path& relative,
    const std::string& expected_sha256) {
    const std::string component_id =
        component.receipt.at("component_id").get<std::string>();
    if (!safe_relative_path(relative) || expected_sha256.size() != 64U) {
        throw std::runtime_error(
            component_id + " public contract reference is unsafe");
    }
    const fs::path normalized = relative.lexically_normal();
    for (const auto& file : component.manifest.at("files")) {
        if (fs::path(file.value("path", "")).lexically_normal() != normalized) {
            continue;
        }
        const fs::path path = component.active_root / normalized;
        if (!fs::is_regular_file(path) || fs::is_symlink(path) ||
            file.value("sha256", "") != expected_sha256 ||
            sha256_file(path) != expected_sha256) {
            throw std::runtime_error(
                component_id + " public contract digest does not match its receipt");
        }
        return path;
    }
    throw std::runtime_error(
        component_id + " public contract is not in its immutable file inventory");
}

struct ProviderServiceContract {
    fs::path executable;
    std::vector<std::string> invocation_prefix;
    json operations;
};

ProviderServiceContract verified_provider_service_contract(
    const fs::path& receipts_root,
    const InstalledComponent& component) {
    if (!component.receipt.contains("service_contract") ||
        !component.receipt.at("service_contract").is_object()) {
        throw std::runtime_error(
            "installed provider has no receipt-verified service contract");
    }
    const json& receipt_contract = component.receipt.at("service_contract");
    if (receipt_contract.value("schema", "") !=
            "uci_arena_service_contract_v1" ||
        !receipt_contract.value("credential_gated_startup", false)) {
        throw std::runtime_error(
            "installed provider service contract is incompatible");
    }
    const auto contract_reference = [&component, &receipt_contract](
                                        const char* path_field,
                                        const char* digest_field) {
        if (!receipt_contract.contains(path_field) ||
            !receipt_contract.at(path_field).is_string() ||
            !receipt_contract.contains(digest_field) ||
            !receipt_contract.at(digest_field).is_string()) {
            throw std::runtime_error(
                "installed provider contract receipt is incomplete");
        }
        const fs::path relative =
            receipt_contract.at(path_field).get<std::string>();
        return std::pair{
            relative,
            verified_component_file(
                component,
                relative,
                receipt_contract.at(digest_field).get<std::string>()),
        };
    };
    const auto [contract_relative, contract_path] =
        contract_reference("path", "sha256");
    if (!component.manifest.contains("contract_manifest") ||
        !component.manifest.at("contract_manifest").is_string() ||
        fs::path(component.manifest.at("contract_manifest").get<std::string>())
                .lexically_normal() != contract_relative.lexically_normal()) {
        throw std::runtime_error(
            "installed provider manifest and receipt contract paths disagree");
    }
    const json contract = read_json_file(contract_path);
    if (contract.value("schema", "") != "uci_arena_service_contract_v1" ||
        !contract.contains("service") || !contract.at("service").is_object()) {
        throw std::runtime_error("installed provider service contract is malformed");
    }
    const json& service = contract.at("service");
    const std::string contract_version = service.value("contract_version", "");
    if (service.value("component_id", "") !=
            component.receipt.value("component_id", "") ||
        service.value("component_version", "") !=
            component.receipt.value("component_version", "") ||
        service.value("transport", "") != "cli_json" ||
        contract_version.size() <= 2 ||
        !contract_version.starts_with("1.") ||
        !std::ranges::all_of(
            std::string_view(contract_version).substr(2),
            [](const unsigned char character) {
                return std::isdigit(character) != 0;
            }) ||
        !service.contains("entrypoint") ||
        !service.at("entrypoint").is_string()) {
        throw std::runtime_error(
            "installed provider service identity is incompatible");
    }
    if (!component.manifest.contains("entrypoints") ||
        !component.manifest.at("entrypoints").is_object() ||
        !component.manifest.at("entrypoints").contains("cli") ||
        component.manifest.at("entrypoints").at("cli") !=
            service.at("entrypoint")) {
        throw std::runtime_error(
            "installed provider CLI entrypoint is not contract-owned");
    }
    const fs::path component_entrypoint = verified_entrypoint(component, "cli");

    const auto verify_referenced_contract = [
                                                &contract,
                                                &contract_reference](
                                                const char* contract_field,
                                                const char* receipt_path_field,
                                                const char* receipt_digest_field) {
        const auto [relative, path] = contract_reference(
            receipt_path_field, receipt_digest_field);
        if (!contract.contains(contract_field) ||
            !contract.at(contract_field).is_string() ||
            fs::path(contract.at(contract_field).get<std::string>())
                    .lexically_normal() != relative.lexically_normal()) {
            throw std::runtime_error(
                "installed provider contract references disagree");
        }
        return path;
    };
    const fs::path operations_path = verify_referenced_contract(
        "manager_operations", "manager_operations_path",
        "manager_operations_sha256");
    static_cast<void>(verify_referenced_contract(
        "configuration_schema", "configuration_schema_path",
        "configuration_schema_sha256"));
    static_cast<void>(verify_referenced_contract(
        "configuration_update_schema", "configuration_update_schema_path",
        "configuration_update_schema_sha256"));
    static_cast<void>(verify_referenced_contract(
        "status_schema", "status_schema_path", "status_schema_sha256"));

    if (!contract.contains("credential_contract") ||
        !contract.at("credential_contract").is_object() ||
        contract.at("credential_contract").value("contents_visible_to", "") !=
            "provider_transport_only" ||
        contract.at("credential_contract").value("manager_projection", "") !=
            "path_and_redacted_status_only" ||
        !contract.contains("error_contract") ||
        !contract.at("error_contract").is_object() ||
        contract.at("error_contract").value("format", "") !=
            "bounded_typed_json" ||
        contract.at("error_contract").value("secrets", "") != "redacted") {
        throw std::runtime_error(
            "installed provider credential or error contract is incompatible");
    }

    const json operations = read_json_file(operations_path);
    const std::string operation_schema = operations.value("schema", "");
    if (!valid_contract_identifier(operation_schema) ||
        !operation_schema.ends_with("_manager_api_v1") ||
        operations.value("version", "") != contract_version ||
        !operations.contains("transport") ||
        !operations.at("transport").is_object() ||
        operations.at("transport").value("kind", "") != "cli_json" ||
        operations.at("transport").value("entrypoint", "") !=
            service.at("entrypoint").get<std::string>() ||
        operations.at("transport").value("stdout", "") !=
            "one_bounded_json_document" ||
        operations.at("transport").value("shell", true) ||
        !operations.contains("operations") ||
        !operations.at("operations").is_object() ||
        operations.at("operations").size() > 64U) {
        throw std::runtime_error(
            "installed provider Manager operation catalog is incompatible");
    }
    for (const auto& [name, operation] : operations.at("operations").items()) {
        if (!valid_contract_identifier(name) || !operation.is_object() ||
            !operation.contains("arguments") ||
            !operation.at("arguments").is_array() ||
            operation.at("arguments").empty() ||
            operation.at("arguments").size() > 8U ||
            !operation.contains("mutates") ||
            !operation.at("mutates").is_boolean() ||
            !operation.contains("credential") ||
            !operation.at("credential").is_string()) {
            throw std::runtime_error(
                "installed provider Manager operation is malformed: " + name);
        }
        const std::string credential =
            operation.at("credential").get<std::string>();
        if (credential != "forbidden" &&
            credential != "external_file_path_only") {
            throw std::runtime_error(
                "installed provider operation credential boundary is invalid: " +
                name);
        }
        for (const auto& argument : operation.at("arguments")) {
            if (!argument.is_string() ||
                !valid_contract_identifier(argument.get<std::string>())) {
                throw std::runtime_error(
                    "installed provider operation argument is invalid: " + name);
            }
        }
    }
    const std::set<std::string> required_operations{
        "account_probe", "config_get", "config_update", "owner_add",
        "owner_list", "owner_remove", "provider_status",
        "reservation_cancel", "reservation_create", "reservation_status",
        "scheduler_pause", "scheduler_resume", "scheduler_status",
        "startup_install", "startup_status", "startup_uninstall",
    };
    for (const auto& operation : required_operations) {
        if (!operations.at("operations").contains(operation)) {
            throw std::runtime_error(
                "installed provider is missing required Manager operation: " +
                operation);
        }
    }

    if (!contract.contains("invocation") ||
        !contract.at("invocation").is_object()) {
        throw std::runtime_error("installed provider invocation is missing");
    }
    const json& invocation = contract.at("invocation");
    const std::string invocation_kind = invocation.value("kind", "");
    if (invocation_kind == "direct") {
        return {component_entrypoint, {}, operations};
    }
    if (invocation_kind != "dependency_runtime" ||
        !invocation.contains("runtime_component_id") ||
        !invocation.at("runtime_component_id").is_string() ||
        !invocation.contains("runtime_entrypoint") ||
        !invocation.at("runtime_entrypoint").is_string()) {
        throw std::runtime_error(
            "installed provider invocation kind is incompatible");
    }
    const std::string runtime_component =
        invocation.at("runtime_component_id").get<std::string>();
    const std::string runtime_entrypoint =
        invocation.at("runtime_entrypoint").get<std::string>();
    if (!valid_contract_identifier(runtime_component) ||
        !valid_contract_identifier(runtime_entrypoint) ||
        !component.manifest.contains("dependencies") ||
        !component.manifest.at("dependencies").is_array() ||
        std::ranges::none_of(
            component.manifest.at("dependencies"),
            [&runtime_component](const json& dependency) {
                return dependency.is_string() &&
                       dependency.get_ref<const std::string&>() == runtime_component;
            })) {
        throw std::runtime_error(
            "installed provider runtime is not a declared dependency");
    }
    const InstalledComponent runtime =
        installed_component(receipts_root, runtime_component);
    return {
        verified_entrypoint(runtime, runtime_entrypoint),
        {component_entrypoint.string()},
        operations,
    };
}

std::vector<std::string> provider_operation_arguments(
    const ProviderServiceContract& contract,
    const std::string& operation_name,
    const std::string& expected_credential) {
    if (!contract.operations.at("operations").contains(operation_name)) {
        throw std::runtime_error(
            "provider does not publish the required operation: " + operation_name);
    }
    const json& operation =
        contract.operations.at("operations").at(operation_name);
    if (operation.value("credential", "") != expected_credential) {
        throw std::runtime_error(
            "provider operation credential boundary changed: " + operation_name);
    }
    std::vector<std::string> arguments = contract.invocation_prefix;
    for (const auto& argument : operation.at("arguments")) {
        arguments.push_back(argument.get<std::string>());
    }
    return arguments;
}

struct BotProviderContext {
    InstalledComponent adapter;
    ProviderServiceContract contract;
    fs::path configuration_path;
};

BotProviderContext bot_provider_context(
    const fs::path& manager_data_root) {
    const fs::path receipts_root = manager_data_root / "receipts";
    InstalledComponent adapter =
        installed_component(receipts_root, "lichess.adapter", true);
    ProviderServiceContract contract =
        verified_provider_service_contract(receipts_root, adapter);
    const fs::path configuration_path = fs::absolute(
        adapter.receipt.value("configuration_path", std::string{}))
                                                .lexically_normal();
    if (configuration_path.empty() ||
        !fs::is_regular_file(configuration_path) ||
        fs::is_symlink(configuration_path)) {
        throw std::runtime_error(
            "the installed Lichess adapter configuration is unavailable");
    }
    return {
        std::move(adapter),
        std::move(contract),
        configuration_path,
    };
}

fs::path bot_runtime_directory(const InstalledComponent& adapter) {
    if (!adapter.receipt.contains("data_paths") ||
        !adapter.receipt.at("data_paths").is_array() ||
        adapter.receipt.at("data_paths").empty() ||
        !adapter.receipt.at("data_paths").at(0).is_string()) {
        throw std::runtime_error(
            "the installed Lichess adapter has no managed runtime directory");
    }
    const fs::path runtime = fs::absolute(
        adapter.receipt.at("data_paths").at(0).get<std::string>())
                                 .lexically_normal();
    if (!fs::is_directory(runtime) || fs::is_symlink(runtime)) {
        throw std::runtime_error(
            "the installed Lichess adapter runtime directory is unavailable");
    }
    return runtime;
}

json project_bot_provider_status(const json& result) {
    if (!result.is_object() || result.dump().size() > 64U * 1024U ||
        result.value("schema", "") != "uci_arena_lichess_status_v3" ||
        !result.contains("active") || !result.at("active").is_boolean() ||
        !result.contains("ready") || !result.at("ready").is_boolean() ||
        !result.contains("stream_connected") ||
        !result.at("stream_connected").is_boolean() ||
        !result.contains("blockers") || !result.at("blockers").is_array() ||
        result.at("blockers").size() > 32U) {
        throw std::runtime_error(
            "the provider returned invalid BOT runtime status");
    }
    json blockers = json::array();
    for (const auto& blocker : result.at("blockers")) {
        if (!blocker.is_string() ||
            !valid_contract_identifier(blocker.get<std::string>())) {
            throw std::runtime_error(
                "the provider returned an invalid BOT runtime blocker");
        }
        blockers.push_back(blocker);
    }
    const bool active = result.at("active").get<bool>();
    const bool ready = result.at("ready").get<bool>();
    return {
        {"provider_state", !active ? "inactive" : ready ? "ready" : "degraded"},
        {"provider_active", active},
        {"provider_ready", ready},
        {"stream_connected", result.at("stream_connected")},
        {"blockers", std::move(blockers)},
    };
}

const json& required_object(
    const json& value,
    const std::string& field) {
    if (!value.contains(field) || !value.at(field).is_object()) {
        throw std::runtime_error(
            "the provider returned invalid BOT options");
    }
    return value.at(field);
}

bool bot_live_prerequisites_ready(const json& readiness) {
    static constexpr std::array required{
        "verified_adapter",
        "adapter_runtime",
        "verified_uci_arena_engine",
        "active_model",
        "opening_book",
        "syzygy",
        "timing_profile",
        "single_game_profile",
        "capacity_profile",
        "token_reference",
    };
    return std::ranges::all_of(required, [&readiness](const char* dimension) {
        return readiness.value(dimension, "missing") == "ready";
    });
}

bool valid_qualification_failure_detail(const std::string_view value) {
    if (value.empty() || value.size() > 1024U ||
        value.find('/') != std::string_view::npos ||
        value.find('\\') != std::string_view::npos ||
        std::ranges::any_of(value, [](const unsigned char character) {
            return std::iscntrl(character) != 0;
        })) {
        return false;
    }
    std::string lowercase(value);
    std::ranges::transform(lowercase, lowercase.begin(), [](const unsigned char value) {
        return static_cast<char>(std::tolower(value));
    });
    return std::ranges::none_of(
        std::array{
            std::string_view{"authorization"},
            std::string_view{"bearer"},
            std::string_view{"password"},
            std::string_view{"secret"},
            std::string_view{"token"},
        },
        [&lowercase](const std::string_view marker) {
            return lowercase.find(marker) != std::string::npos;
        });
}

bool valid_prefixed_sha256(const std::string& value) {
    return value.size() == 71U && value.starts_with("sha256:") &&
           std::ranges::all_of(
               std::string_view(value).substr(7),
               [](const unsigned char character) {
                   return std::isdigit(character) != 0 ||
                          (character >= 'a' && character <= 'f');
               });
}

void require_exact_fields(
    const json& source,
    const std::initializer_list<std::string_view> fields) {
    if (!source.is_object() || source.size() != fields.size()) {
        throw std::runtime_error(
            "the provider returned undeclared BOT option fields");
    }
    for (const auto field : fields) {
        if (!source.contains(std::string(field))) {
            throw std::runtime_error(
                "the provider returned incomplete BOT options");
        }
    }
}

json selected_fields(
    const json& source,
    const std::initializer_list<std::string_view> fields) {
    json result = json::object();
    for (const auto field : fields) {
        const std::string key(field);
        if (!source.contains(key)) {
            throw std::runtime_error(
                "the provider returned incomplete BOT options");
        }
        result[key] = source.at(key);
    }
    return result;
}

int required_integer(
    const json& source,
    const std::string& field,
    const int minimum,
    const int maximum) {
    if (!source.contains(field) ||
        !source.at(field).is_number_integer()) {
        throw std::runtime_error(
            "the provider returned an invalid BOT option: " + field);
    }
    const int value = source.at(field).get<int>();
    if (value < minimum || value > maximum) {
        throw std::runtime_error(
            "the provider returned an out-of-range BOT option: " + field);
    }
    return value;
}

bool required_boolean(const json& source, const std::string& field) {
    if (!source.contains(field) || !source.at(field).is_boolean()) {
        throw std::runtime_error(
            "the provider returned an invalid BOT option: " + field);
    }
    return source.at(field).get<bool>();
}

void require_string_choice(
    const json& source,
    const std::string& field,
    const std::set<std::string>& choices) {
    if (!source.contains(field) || !source.at(field).is_string() ||
        !choices.contains(source.at(field).get<std::string>())) {
        throw std::runtime_error(
            "the provider returned an invalid BOT option: " + field);
    }
}

void require_string_choices(
    const json& source,
    const std::string& field,
    const std::set<std::string>& choices) {
    if (!source.contains(field) || !source.at(field).is_array() ||
        source.at(field).empty() ||
        source.at(field).size() > choices.size()) {
        throw std::runtime_error(
            "the provider returned an invalid BOT option: " + field);
    }
    std::set<std::string> observed;
    for (const auto& entry : source.at(field)) {
        if (!entry.is_string() ||
            !choices.contains(entry.get<std::string>()) ||
            !observed.insert(entry.get<std::string>()).second) {
            throw std::runtime_error(
                "the provider returned an invalid BOT option: " + field);
        }
    }
}

void validate_projected_bot_options(const json& options) {
    require_exact_fields(
        options,
        {"schema", "capacity", "inbound_challenges",
         "outbound_challenge", "scheduler", "opponent_selection"});
    if (!options.at("schema").is_string() ||
        options.at("schema") != "uci_arena_lichess_manager_options_v1") {
        throw std::runtime_error(
            "the provider returned an unsupported BOT options schema");
    }
    const json& capacity = required_object(options, "capacity");
    require_exact_fields(
        capacity,
        {"total_slots", "startup_parallelism", "human", "bot",
         "evidence"});
    const int total_slots =
        required_integer(capacity, "total_slots", 1, 64);
    required_integer(capacity, "startup_parallelism", 1, 64);
    const json& human = required_object(capacity, "human");
    const json& bot = required_object(capacity, "bot");
    require_exact_fields(
        human, {"slots", "allow_borrowing", "accept_public"});
    require_exact_fields(bot, {"slots", "target_active"});
    const int human_slots = required_integer(human, "slots", 0, 64);
    const int bot_slots = required_integer(bot, "slots", 0, 64);
    if (human_slots + bot_slots != total_slots) {
        throw std::runtime_error(
            "the provider returned inconsistent BOT capacity");
    }
    static_cast<void>(required_boolean(human, "allow_borrowing"));
    static_cast<void>(required_boolean(human, "accept_public"));
    required_integer(bot, "target_active", 0, bot_slots);
    const json& evidence = required_object(capacity, "evidence");
    require_exact_fields(
        evidence, {"status", "qualified", "maximum_concurrency"});
    static const std::set<std::string> evidence_statuses{
        "legacy_shared", "standard_uci_startup_probe_required",
        "runtime_revalidation_required", "missing", "qualified",
        "rejected"};
    if (!evidence.contains("status") || !evidence.at("status").is_string() ||
        !evidence_statuses.contains(evidence.at("status").get<std::string>()) ||
        !evidence.contains("qualified") ||
        !evidence.at("qualified").is_boolean() ||
        !evidence.contains("maximum_concurrency") ||
        !(evidence.at("maximum_concurrency").is_null() ||
          evidence.at("maximum_concurrency").is_number_integer())) {
        throw std::runtime_error(
            "the provider returned invalid BOT capacity evidence");
    }
    if (evidence.at("qualified").get<bool>()) {
        const int maximum = required_integer(
            evidence, "maximum_concurrency", 1, 64);
        if (total_slots > maximum) {
            throw std::runtime_error(
                "the provider BOT capacity exceeds qualified evidence");
        }
    } else if (!evidence.at("maximum_concurrency").is_null()) {
        throw std::runtime_error(
            "unqualified BOT capacity evidence exposed a maximum");
    }

    const json& inbound = required_object(options, "inbound_challenges");
    require_exact_fields(
        inbound,
        {"accept_bots", "min_base", "max_base", "min_increment",
         "max_increment", "speeds", "modes"});
    static_cast<void>(required_boolean(inbound, "accept_bots"));
    const int minimum_base =
        required_integer(inbound, "min_base", 0, 86400);
    const int maximum_base =
        required_integer(inbound, "max_base", 0, 86400);
    const int minimum_increment =
        required_integer(inbound, "min_increment", 0, 300);
    const int maximum_increment =
        required_integer(inbound, "max_increment", 0, 300);
    if (minimum_base > maximum_base ||
        minimum_increment > maximum_increment) {
        throw std::runtime_error(
            "the provider returned inconsistent BOT challenge bounds");
    }
    require_string_choices(
        inbound,
        "speeds",
        {"ultraBullet", "bullet", "blitz", "rapid", "classical",
         "correspondence"});
    require_string_choices(inbound, "modes", {"casual", "rated"});

    const json& outbound = required_object(options, "outbound_challenge");
    require_exact_fields(
        outbound, {"rated", "color", "clock_limit", "clock_increment"});
    static_cast<void>(required_boolean(outbound, "rated"));
    require_string_choice(
        outbound, "color", {"random", "white", "black"});
    required_integer(outbound, "clock_limit", 0, 86400);
    required_integer(outbound, "clock_increment", 0, 300);

    const json& scheduler = required_object(options, "scheduler");
    require_exact_fields(
        scheduler,
        {"enabled", "discovery_interval_seconds",
         "challenge_interval_seconds", "reservation_retry_seconds",
         "challenge_timeout_seconds", "first_move_timeout_seconds"});
    static_cast<void>(required_boolean(scheduler, "enabled"));
    required_integer(scheduler, "discovery_interval_seconds", 15, 3600);
    required_integer(scheduler, "challenge_interval_seconds", 1, 300);
    required_integer(scheduler, "reservation_retry_seconds", 5, 600);
    required_integer(scheduler, "challenge_timeout_seconds", 10, 300);
    required_integer(scheduler, "first_move_timeout_seconds", 30, 600);
    const json& opponents = required_object(options, "opponent_selection");
    require_exact_fields(
        opponents,
        {"max_games_per_opponent_per_day", "timeout_cooldown_minutes",
         "decline_cooldown_minutes", "rating_target", "rating_spread"});
    required_integer(opponents, "max_games_per_opponent_per_day", 1, 100);
    required_integer(opponents, "timeout_cooldown_minutes", 1, 1440);
    required_integer(opponents, "decline_cooldown_minutes", 1, 10080);
    required_integer(opponents, "rating_target", 400, 4000);
    required_integer(opponents, "rating_spread", 50, 2000);
}

json project_bot_options(const json& result) {
    if (!result.is_object() || result.dump().size() > 32U * 1024U) {
        throw std::runtime_error(
            "the provider returned invalid BOT options");
    }
    validate_projected_bot_options(result);
    return result;
}

void require_parameter_keys(
    const json& parameters,
    const std::set<std::string>& allowed) {
    if (!parameters.is_object()) {
        throw std::invalid_argument(
            "BOT account-control parameters must be an object");
    }
    for (const auto& [key, value] : parameters.items()) {
        static_cast<void>(value);
        if (!allowed.contains(key)) {
            throw std::invalid_argument(
                "unsupported BOT account-control parameter");
        }
    }
}

int bounded_integer_parameter(
    const json& parameters,
    const std::string& name,
    const int minimum,
    const int maximum) {
    if (!parameters.contains(name) ||
        !parameters.at(name).is_number_integer()) {
        throw std::invalid_argument(name + " must be an integer");
    }
    const int value = parameters.at(name).get<int>();
    if (value < minimum || value > maximum) {
        throw std::invalid_argument(name + " is outside the allowed range");
    }
    return value;
}

std::string account_parameter(
    const json& parameters,
    const std::string& name) {
    if (!parameters.contains(name) ||
        !parameters.at(name).is_string()) {
        throw std::invalid_argument(name + " is required");
    }
    const std::string value = parameters.at(name).get<std::string>();
    if (!valid_lichess_account_name(value)) {
        throw std::invalid_argument(name + " is not a valid Lichess account");
    }
    return value;
}

bool compatible_manager_api_version(const std::string_view version) {
    return version.size() > 2 && version.starts_with("1.") &&
           std::ranges::all_of(
               version.substr(2),
               [](const unsigned char character) {
                   return std::isdigit(character) != 0;
               });
}

bool manager_service_api_required(const std::string_view version) {
    std::array<unsigned int, 3> parts{};
    std::size_t part = 0;
    bool digit = false;
    for (const unsigned char character : version) {
        if (character == '.') {
            if (!digit || part >= parts.size() - 1) {
                throw std::runtime_error(
                    "installed service component version is not semantic");
            }
            ++part;
            digit = false;
        } else if (std::isdigit(character) != 0) {
            parts[part] = parts[part] * 10 +
                          static_cast<unsigned int>(character - '0');
            digit = true;
        } else {
            throw std::runtime_error(
                "installed service component version is not semantic");
        }
    }
    if (!digit || part != 2) {
        throw std::runtime_error(
            "installed service component version is not semantic");
    }
    return parts[0] > 0 || parts[1] >= 3;
}

json normalized_manager_service_api(const json& declaration) {
    if (!declaration.is_object() ||
        declaration.value("schema", "") !=
            "uci_arena_manager_service_api_v1" ||
        !declaration.contains("version") ||
        !declaration.at("version").is_string() ||
        !compatible_manager_api_version(
            declaration.at("version").get_ref<const std::string&>()) ||
        declaration.value("transport", "") != "cli_json" ||
        declaration.value("entrypoint", "") != "cli" ||
        declaration.value("output_schema", "") !=
            "uci_arena_service_output_v1" ||
        declaration.value("arguments", json::array()) !=
            json::array({"api"})) {
        throw std::runtime_error(
            "installed service Manager API declaration is incompatible");
    }
    const std::set<std::string> expected{
        "config_validate",
        "describe",
        "outputs_inspect",
        "outputs_list",
        "status",
    };
    if (!declaration.contains("operations") ||
        !declaration.at("operations").is_array()) {
        throw std::runtime_error(
            "installed service Manager API operations are missing");
    }
    std::set<std::string> operations;
    for (const auto& operation : declaration.at("operations")) {
        if (!operation.is_string() ||
            !operations.insert(operation.get<std::string>()).second) {
            throw std::runtime_error(
                "installed service Manager API operations are malformed");
        }
    }
    if (!std::ranges::includes(operations, expected)) {
        throw std::runtime_error(
            "installed service Manager API operations are incomplete");
    }
    return declaration;
}

json manual_manager_service_api() {
    return {
        {"schema", "uci_arena_manager_service_api_v1"},
        {"version", "1.0"},
        {"transport", "cli_json"},
        {"entrypoint", "cli"},
        {"arguments", {"api"}},
        {"operations",
         {"describe",
          "status",
          "outputs_list",
          "outputs_inspect",
          "config_validate"}},
        {"output_schema", "uci_arena_service_output_v1"},
    };
}

std::optional<json> verified_manager_service_api(
    const InstalledComponent& component) {
    if (!component.manifest.contains("manager_integration_api")) {
        if (manager_service_api_required(
                component.receipt.at("component_version")
                    .get_ref<const std::string&>())) {
            throw std::runtime_error(
                "installed service is missing its signed Manager API declaration");
        }
        return std::nullopt;
    }
    const json declaration = normalized_manager_service_api(
        component.manifest.at("manager_integration_api"));
    if (declaration.at("entrypoint").get<std::string>() != "cli") {
        throw std::runtime_error(
            "installed service Manager API entrypoint is incompatible");
    }
    static_cast<void>(verified_entrypoint(component, "cli"));
    return std::optional<json>{declaration};
}

json book_forge_profile() {
    return {
        {"profile_id", "book_forge"},
        {"display_name", "Book Forge"},
        {"implementation_state", "implemented_external"},
        {"optional", true},
        {"description", "Continuous opening-book acquisition, analysis, and publication"},
        {"capabilities",
         {
             "status",
             "start",
             "stop",
             "restart",
             "pause",
             "resume",
             "acquisition_status",
             "acquisition_run",
             "acquisition_retry_auth",
             "snapshot_build",
             "validate",
             "publish",
             "maintain",
             "startup_install",
             "startup_status",
             "startup_recover",
             "startup_uninstall",
         }},
        {"required_configuration", {"node_executable", "cli_entrypoint", "workspace"}},
        {"discovery",
         {
             {"supported", true},
             {"manual_fallback", true},
             {"sources", {"process_environment_hints", "executable_path"}},
         }},
        {"health_contract",
         {
             {"schema", "uci_arena_book_forge_status_v2"},
             {"heartbeat_seconds", 15},
             {"stale_seconds", 300},
         }},
    };
}

json evidence_service_profile() {
    return {
        {"profile_id", "uci_arena_evidence"},
        {"display_name", "UCI Arena Evidence Service"},
        {"implementation_state", "implemented_external"},
        {"optional", true},
        {"description",
         "Durable identity-bound timing, strength evidence, datasets, and qualified profiles"},
        {"capabilities",
         {
             "status",
             "start",
             "stop",
             "restart",
             "pause",
             "resume",
             "init",
             "target_add",
             "target_list",
             "target_remove",
             "job_enqueue",
             "job_list",
             "job_cancel",
             "job_retry",
             "dataset_list",
             "dataset_show",
             "dataset_export",
             "profile_list",
             "prune",
             "startup_install",
             "startup_status",
             "startup_recover",
             "startup_uninstall",
         }},
        {"required_configuration", {"node_executable", "cli_entrypoint", "workspace"}},
        {"discovery",
         {
             {"supported", true},
             {"manual_fallback", true},
             {"sources", {"process_environment_hints", "executable_path"}},
         }},
        {"health_contract",
         {
             {"schema", "uci_arena_evidence_status_v1"},
             {"heartbeat_seconds", 15},
             {"stale_seconds", 300},
         }},
    };
}

std::filesystem::path required_path(const json& object, const char* key) {
    if (!object.contains(key) || !object.at(key).is_string() ||
        object.at(key).get<std::string>().empty()) {
        throw std::runtime_error(std::string("service configuration requires ") + key);
    }
    return object.at(key).get<std::string>();
}

std::optional<std::string> environment_value(const char* key) {
#ifdef _WIN32
    char* buffer = nullptr;
    std::size_t length = 0;
    if (_dupenv_s(&buffer, &length, key) != 0 || buffer == nullptr) {
        return std::nullopt;
    }
    std::string value(buffer);
    std::free(buffer);
    if (value.empty()) {
        return std::nullopt;
    }
    return value;
#else
    const char* value = std::getenv(key);
    if (value == nullptr || *value == '\0') {
        return std::nullopt;
    }
    return std::string(value);
#endif
}

std::optional<std::filesystem::path> executable_on_path(const std::string& name) {
    const auto path_value = environment_value("PATH");
    if (!path_value) {
        return std::nullopt;
    }
#ifdef _WIN32
    constexpr char separator = ';';
    const std::vector<std::string> names{name + ".exe", name};
#else
    constexpr char separator = ':';
    const std::vector<std::string> names{name};
#endif
    std::size_t begin = 0;
    while (begin <= path_value->size()) {
        const auto end = path_value->find(separator, begin);
        const std::filesystem::path directory =
            path_value->substr(begin, end == std::string::npos ? end : end - begin);
        if (!directory.empty()) {
            for (const auto& candidate_name : names) {
                const auto candidate = directory / candidate_name;
                if (std::filesystem::is_regular_file(candidate)) {
                    return std::filesystem::absolute(candidate);
                }
            }
        }
        if (end == std::string::npos) {
            break;
        }
        begin = end + 1;
    }
    return std::nullopt;
}

struct SavedServiceLocator {
    std::optional<fs::path> cli_entrypoint;
    std::optional<std::string> warning;
};

SavedServiceLocator saved_service_locator(
    const fs::path& manager_data_root,
    const std::string& kind,
    const std::string& component_id) {
    if (manager_data_root.empty()) {
        return {};
    }
    const fs::path locator_path =
        manager_data_root / "locators" / (kind + ".json");
    if (!fs::exists(locator_path)) {
        return {};
    }
    try {
        if (!fs::is_regular_file(locator_path) ||
            fs::is_symlink(locator_path) ||
            fs::file_size(locator_path) > 1024ULL * 1024ULL) {
            throw std::runtime_error("saved locator metadata is unsafe");
        }
        const json locator = read_json_file(locator_path);
        if (locator.value("schema", "") != "arena_saved_locator_v1" ||
            locator.value("schema_version", 0) != 1 ||
            locator.value("kind", "") != kind ||
            locator.value("mode", "") != "in_place_reference" ||
            !locator.contains("path") || !locator.at("path").is_string()) {
            throw std::runtime_error("saved locator metadata is incompatible");
        }
        const fs::path target =
            fs::absolute(locator.at("path").get<std::string>()).lexically_normal();
        if (fs::is_symlink(target)) {
            throw std::runtime_error("saved locator target is a symbolic link");
        }
        if (fs::is_regular_file(target)) {
            return {target, std::nullopt};
        }
        if (!fs::is_directory(target)) {
            throw std::runtime_error("saved locator target is unavailable");
        }
        const fs::path manifest_path = target / "arena-component.json";
        if (!fs::is_regular_file(manifest_path) ||
            fs::is_symlink(manifest_path) ||
            fs::file_size(manifest_path) > 1024ULL * 1024ULL) {
            throw std::runtime_error(
                "saved provider root has no safe component manifest");
        }
        const json manifest = read_json_file(manifest_path);
        if (manifest.value("schema", "") != "arena_install_component_v1" ||
            manifest.value("component_id", "") != component_id ||
            !manifest.contains("entrypoints") ||
            !manifest.at("entrypoints").is_object() ||
            !manifest.at("entrypoints").contains("cli") ||
            !manifest.at("entrypoints").at("cli").is_string()) {
            throw std::runtime_error(
                "saved provider manifest has an incompatible CLI contract");
        }
        const fs::path relative =
            manifest.at("entrypoints").at("cli").get<std::string>();
        if (!safe_relative_path(relative)) {
            throw std::runtime_error(
                "saved provider manifest has an unsafe CLI entrypoint");
        }
        const fs::path cli = target / relative;
        if (!within(target, cli) || !fs::is_regular_file(cli) ||
            fs::is_symlink(cli)) {
            throw std::runtime_error(
                "saved provider CLI entrypoint is unavailable");
        }
        return {fs::absolute(cli).lexically_normal(), std::nullopt};
    } catch (const std::exception& error) {
        return {std::nullopt, error.what()};
    }
}

}  // namespace

BotQualificationFailure::BotQualificationFailure(
    const std::string& code,
    const std::string& stage,
    const std::string& detail)
    : std::runtime_error(detail), code_(code), stage_(stage), detail_(detail) {}

const std::string& BotQualificationFailure::code() const noexcept {
    return code_;
}

const std::string& BotQualificationFailure::stage() const noexcept {
    return stage_;
}

const std::string& BotQualificationFailure::detail() const noexcept {
    return detail_;
}

json ServiceCatalog::profiles() {
    return json::array({book_forge_profile(), evidence_service_profile()});
}

json ServiceCatalog::discover(
    const std::string& profile_id,
    const std::filesystem::path& manager_data_root) {
    json candidates = json::array();
    json manual_configuration = json::array();
    if (profile_id == "book_forge") {
        manual_configuration = {"node_executable", "cli_entrypoint", "workspace"};
        json configuration = json::object();
        const auto saved = saved_service_locator(
            manager_data_root, "book_forge", "book_forge.service");
        if (saved.cli_entrypoint) {
            configuration["cli_entrypoint"] =
                saved.cli_entrypoint->string();
        }
        if (const auto configured_node = environment_value("BOOK_FORGE_NODE")) {
            configuration["node_executable"] = *configured_node;
        } else if (const auto path_node = executable_on_path("node")) {
            configuration["node_executable"] = path_node->string();
        }
        if (const auto cli = environment_value("BOOK_FORGE_CLI")) {
            configuration["cli_entrypoint"] = *cli;
        }
        if (const auto workspace = environment_value("BOOK_FORGE_WORKSPACE")) {
            configuration["workspace"] = *workspace;
        }
        if (!configuration.empty()) {
            json missing = json::array();
            for (const auto* key : {"node_executable", "cli_entrypoint", "workspace"}) {
                if (!configuration.contains(key)) {
                    missing.push_back(key);
                }
            }
            candidates.push_back(
                {
                    {"source",
                     saved.cli_entrypoint || saved.warning
                         ? "saved_locator"
                         : "environment_and_path"},
                    {"complete", missing.empty()},
                    {"configuration", configuration},
                    {"missing", missing},
                    {"warning", saved.warning ? json(*saved.warning) : json(nullptr)},
                });
        } else if (saved.warning) {
            candidates.push_back(
                {
                    {"source", "saved_locator"},
                    {"complete", false},
                    {"configuration", configuration},
                    {"missing", manual_configuration},
                    {"warning", *saved.warning},
                });
        }
    } else if (profile_id == "uci_arena_evidence") {
        manual_configuration = {"node_executable", "cli_entrypoint", "workspace"};
        json configuration = json::object();
        const auto saved = saved_service_locator(
            manager_data_root,
            "evidence_service",
            "uci_arena_evidence.service");
        if (saved.cli_entrypoint) {
            configuration["cli_entrypoint"] =
                saved.cli_entrypoint->string();
        }
        if (const auto configured_node = environment_value("UCI_ARENA_EVIDENCE_NODE")) {
            configuration["node_executable"] = *configured_node;
        } else if (const auto path_node = executable_on_path("node")) {
            configuration["node_executable"] = path_node->string();
        }
        if (const auto cli = environment_value("UCI_ARENA_EVIDENCE_CLI")) {
            configuration["cli_entrypoint"] = *cli;
        }
        if (const auto workspace = environment_value("UCI_ARENA_EVIDENCE_ROOT")) {
            configuration["workspace"] = *workspace;
        }
        if (!configuration.empty()) {
            json missing = json::array();
            for (const auto* key : {"node_executable", "cli_entrypoint", "workspace"}) {
                if (!configuration.contains(key)) {
                    missing.push_back(key);
                }
            }
            candidates.push_back(
                {
                    {"source",
                     saved.cli_entrypoint || saved.warning
                         ? "saved_locator"
                         : "environment_and_path"},
                    {"complete", missing.empty()},
                    {"configuration", configuration},
                    {"missing", missing},
                    {"warning", saved.warning ? json(*saved.warning) : json(nullptr)},
                });
        } else if (saved.warning) {
            candidates.push_back(
                {
                    {"source", "saved_locator"},
                    {"complete", false},
                    {"configuration", configuration},
                    {"missing", manual_configuration},
                    {"warning", *saved.warning},
                });
        }
    } else {
        throw std::runtime_error("unknown managed service profile: " + profile_id);
    }
    return {
        {"schema_version", 1},
        {"profile_id", profile_id},
        {"optional", true},
        {"candidates", candidates},
        {"manual_configuration", manual_configuration},
    };
}

json ServiceCatalog::normalize_registration(const json& parameters) {
    if (!parameters.is_object()) {
        throw std::runtime_error("service parameters must be an object");
    }
    const std::string profile_id = parameters.value("profile_id", "");
    json data = parameters;
    data.erase("labels");
    data.erase("manager_integration_api");
    data.erase("manager_integration_api_source");
    if (parameters.contains("manager_integration_api")) {
        data["manager_integration_api"] = normalized_manager_service_api(
            parameters.at("manager_integration_api"));
        data["manager_integration_api_source"] =
            parameters.value(
                "manager_integration_api_source",
                "signed_component_manifest");
    } else if (!parameters.contains("installed_component_id")) {
        data["manager_integration_api"] = manual_manager_service_api();
        data["manager_integration_api_source"] =
            "manual_profile_expectation";
    }
    const bool startup_recovery_declared =
        data.contains("manager_integration_api") &&
        data.at("manager_integration_api").is_object() &&
        data.at("manager_integration_api").contains("operations") &&
        data.at("manager_integration_api").at("operations").is_array() &&
        std::ranges::any_of(
            data.at("manager_integration_api").at("operations"),
            [](const json& operation) {
                return operation.is_string() &&
                       operation.get_ref<const std::string&>() ==
                           "startup_recover";
            });
    const auto remove_capability = [&data](const char* capability) {
        const auto iterator = std::find(
            data["capabilities"].begin(),
            data["capabilities"].end(),
            capability);
        if (iterator != data["capabilities"].end()) {
            data["capabilities"].erase(iterator);
        }
    };
    if (profile_id == "book_forge") {
        const auto node = required_path(parameters, "node_executable");
        const auto entrypoint = required_path(parameters, "cli_entrypoint");
        const auto workspace = required_path(parameters, "workspace");
        if (!std::filesystem::is_regular_file(node)) {
            throw std::runtime_error("Book Forge Node executable is unavailable");
        }
        if (!std::filesystem::is_regular_file(entrypoint)) {
            throw std::runtime_error("Book Forge CLI entrypoint is unavailable");
        }
        if (!std::filesystem::is_directory(workspace)) {
            throw std::runtime_error("Book Forge workspace is unavailable");
        }
        data["node_executable"] = std::filesystem::absolute(node).string();
        data["cli_entrypoint"] = std::filesystem::absolute(entrypoint).string();
        data["workspace"] = std::filesystem::absolute(workspace).string();
        data["display_name"] = parameters.value("display_name", "Book Forge");
        data["integration_state"] = "configured";
        data["desired_state"] =
            parameters.value("startup_installed", false) ? "running" : "stopped";
        data["observed_state"] = "stopped";
        data["transport"] = "process";
        data["capabilities"] = book_forge_profile().at("capabilities");
        if (parameters.value("startup_installed", false)) {
            data["startup_control"] = "provider_owned_per_user";
            for (const auto* capability : {"start", "stop", "restart"}) {
                remove_capability(capability);
            }
        }
        if (!parameters.value("startup_installed", false) ||
            !startup_recovery_declared) {
            remove_capability("startup_recover");
        }
        data["health"] = {
            {"state", "unknown"},
            {"reason", "status has not been probed"},
            {"stale_after_seconds", 300},
        };
        return data;
    }
    if (profile_id == "uci_arena_evidence") {
        const auto node = required_path(parameters, "node_executable");
        const auto entrypoint = required_path(parameters, "cli_entrypoint");
        const auto workspace = required_path(parameters, "workspace");
        if (!std::filesystem::is_regular_file(node)) {
            throw std::runtime_error("UCI Arena Evidence Service Node executable is unavailable");
        }
        if (!std::filesystem::is_regular_file(entrypoint)) {
            throw std::runtime_error("UCI Arena Evidence Service CLI entrypoint is unavailable");
        }
        if (std::filesystem::exists(workspace) && !std::filesystem::is_directory(workspace)) {
            throw std::runtime_error("UCI Arena Evidence Service workspace is not a directory");
        }
        data["display_name"] =
            parameters.value("display_name", "UCI Arena Evidence Service");
        data["node_executable"] = std::filesystem::absolute(node).string();
        data["cli_entrypoint"] = std::filesystem::absolute(entrypoint).string();
        data["workspace"] = std::filesystem::absolute(workspace).string();
        data["integration_state"] = "configured";
        data["desired_state"] =
            parameters.value("startup_installed", false) ? "running" : "stopped";
        data["observed_state"] = "stopped";
        data["transport"] = "process";
        data["capabilities"] = evidence_service_profile().at("capabilities");
        if (parameters.value("startup_installed", false)) {
            data["startup_control"] = "provider_owned_per_user";
            for (const auto* capability : {"start", "stop", "restart"}) {
                remove_capability(capability);
            }
        }
        if (!parameters.value("startup_installed", false) ||
            !startup_recovery_declared) {
            remove_capability("startup_recover");
        }
        data["health"] = {
            {"state", "unknown"},
            {"reason", "status has not been probed"},
            {"stale_after_seconds", 300},
        };
        return data;
    }
    throw std::runtime_error("unknown managed service profile: " + profile_id);
}

json ServiceCatalog::installed_registrations(
    const std::filesystem::path& manager_data_root) {
    const fs::path receipts_root = manager_data_root / "receipts";
    json services = json::array();
    json errors = json::array();
    if (!fs::is_directory(receipts_root)) {
        return {
            {"schema", "arena_installed_service_registrations_v1"},
            {"services", std::move(services)},
            {"errors", std::move(errors)},
        };
    }

    std::optional<InstalledComponent> node_runtime;
    try {
        node_runtime =
            installed_component(receipts_root, "node_runtime.private");
    } catch (const std::exception& error) {
        errors.push_back(
            {{"component_id", "node_runtime.private"},
             {"error", error.what()}});
    }
    for (const auto& [component_id, profile_id, display_name] :
         std::array{
             std::array<std::string, 3>{
                 "book_forge.service", "book_forge", "Book Forge"},
             std::array<std::string, 3>{
                 "uci_arena_evidence.service",
                 "uci_arena_evidence",
                 "UCI Arena Evidence Service"}}) {
        if (!fs::is_regular_file(receipts_root / (component_id + ".json"))) {
            continue;
        }
        try {
            if (!node_runtime.has_value()) {
                throw std::runtime_error(
                    "private Node runtime is unavailable");
            }
            const InstalledComponent service =
                installed_component(receipts_root, component_id);
            const auto manager_service_api =
                verified_manager_service_api(service);
            if (!service.receipt.contains("data_paths") ||
                !service.receipt.at("data_paths").is_array() ||
                service.receipt.at("data_paths").empty()) {
                throw std::runtime_error(
                    "installed service receipt has no workspace");
            }
            const fs::path workspace =
                service.receipt.at("data_paths").at(0).get<std::string>();
            if (!fs::is_directory(workspace)) {
                throw std::runtime_error(
                    "installed service workspace is unavailable");
            }
            json registration{
                {"profile_id", profile_id},
                {"display_name", display_name},
                {"node_executable",
                 verified_entrypoint(*node_runtime, "node").string()},
                {"cli_entrypoint",
                 verified_entrypoint(service, "cli").string()},
                {"workspace", workspace.string()},
                {"installed_component_id", component_id},
                {"installed_component_version",
                 service.receipt.at("component_version")},
                {"installed_artifact_sha256",
                 service.receipt.at("artifact_sha256")},
                {"runtime_component_id", "node_runtime.private"},
                {"runtime_artifact_sha256",
                 node_runtime->receipt.at("artifact_sha256")},
                {"startup_installed",
                 service.receipt.value("startup", json::object())
                     .value("installed", false)},
            };
            if (manager_service_api.has_value()) {
                registration["manager_integration_api"] =
                    *manager_service_api;
                registration["manager_integration_api_source"] =
                    "signed_component_manifest";
            }
            const std::string configuration_path =
                service.receipt.value("configuration_path", "");
            if (!configuration_path.empty()) {
                const fs::path configuration = configuration_path;
                if (!within(workspace, configuration) ||
                    !fs::is_regular_file(configuration) ||
                    fs::is_symlink(configuration)) {
                    throw std::runtime_error(
                        "installed service configuration is unavailable or unsafe");
                }
                registration["config_path"] =
                    fs::absolute(configuration).string();
            }
            services.push_back(normalize_registration(registration));
        } catch (const std::exception& error) {
            errors.push_back(
                {{"component_id", component_id}, {"error", error.what()}});
        }
    }
    return {
        {"schema", "arena_installed_service_registrations_v1"},
        {"services", std::move(services)},
        {"errors", std::move(errors)},
    };
}

json ServiceCatalog::component_inventory(
    const fs::path& manager_data_root) {
    const fs::path receipts_root = manager_data_root / "receipts";
    const std::set<std::string> downloadable =
        retained_catalog_components(manager_data_root);
    json components = json::array();
    for (const auto& [component_id, display_name, optional] :
         std::array{
             std::tuple{"uci_arena.engine", "UCI Arena Engine", false},
             std::tuple{"stockfish.engine", "Stockfish", false},
             std::tuple{"node_runtime.private", "Private Node runtime", false},
             std::tuple{"book_forge.service", "Book Forge", false},
             std::tuple{"opening_book.standard", "Opening book", false},
             std::tuple{"uci_arena_evidence.service",
                        "UCI Arena Evidence Service", false},
             std::tuple{"lichess.adapter", "Lichess Bot adapter", true}}) {
        json row{
            {"component_id", component_id},
            {"display_name", display_name},
            {"optional", optional},
            {"package_state", "missing"},
            {"downloadable", downloadable.contains(component_id)},
        };
        const fs::path receipt_path =
            receipts_root / (std::string(component_id) + ".json");
        if (fs::is_regular_file(receipt_path)) {
            try {
                const InstalledComponent component =
                    installed_component(receipts_root, component_id, true);
                row["package_state"] = "installed";
                row["installation_state"] =
                    component.receipt.value("state", "");
                row["component_version"] =
                    component.receipt.value("component_version", "");
                row["startup_installed"] =
                    component.receipt.value("startup", json::object())
                        .value("installed", false);
                if (component.manifest.contains("default_model") &&
                    component.manifest.at("default_model").is_object()) {
                    row["default_model"] =
                        component.manifest.at("default_model");
                }
            } catch (const std::exception& error) {
                row["package_state"] = "broken";
                row["issue"] = error.what();
            }
        }
        components.push_back(std::move(row));
    }
    return {
        {"schema", "arena_component_inventory_v1"},
        {"schema_version", 1},
        {"retained_catalog_available", !downloadable.empty()},
        {"components", std::move(components)},
    };
}

json ServiceCatalog::bot_profiles() {
    return json::array({
        {
            {"profile_id", "lichess_bot"},
            {"display_name", "Lichess Bot"},
            {"adapter_component_id", "lichess.adapter"},
            {"optional", true},
            {"capabilities",
             {"account_probe", "startup_status", "enable_startup",
              "disable_startup", "installed_qualification", "options",
              "owners", "reservations", "scheduler", "challenge_policy"}},
        },
    });
}

json ServiceCatalog::discover_bot(
    const fs::path& manager_data_root) {
    const fs::path receipts_root = manager_data_root / "receipts";
    json issues = json::array();
    json readiness = json::object();
    json candidate{
        {"candidate_id", "botcand-lichess-local"},
        {"profile_id", "lichess_bot"},
        {"display_name", "Lichess Bot"},
        {"adapter_component_id", "lichess.adapter"},
        {"package_state", "missing"},
        {"integration_state", "not_configured"},
        {"token_reference_available", false},
        {"configuration_state", "missing"},
        {"startup_state", "disabled"},
        {"qualification_state", "unavailable"},
        {"options_state", "interactive_helper_required"},
    };
    bool adapter_ready = false;
    bool configuration_ready = false;
    bool startup_installed = false;
    std::optional<fs::path> adapter_root;
    json configuration = json::object();
    try {
        const InstalledComponent adapter =
            installed_component(receipts_root, "lichess.adapter", true);
        candidate["package_state"] = "installed";
        candidate["component_version"] =
            adapter.receipt.value("component_version", "");
        adapter_root = adapter.active_root;
        try {
            const ProviderServiceContract contract =
                verified_provider_service_contract(receipts_root, adapter);
            candidate["integration_state"] = "compatible";
            candidate["qualification_state"] =
                contract.operations.at("operations").contains("qualification_run")
                    ? "available"
                    : "unavailable";
            adapter_ready = true;
        } catch (const std::exception& error) {
            candidate["integration_state"] = "incompatible";
            issues.push_back({
                {"code", "bot_adapter_incompatible"},
                {"detail", error.what()},
            });
        }
        const std::string configuration_path =
            adapter.receipt.value("configuration_path", "");
        if (configuration_path.empty() ||
            !fs::is_regular_file(configuration_path) ||
            fs::is_symlink(configuration_path)) {
            issues.push_back({
                {"code", "bot_configuration_missing"},
                {"detail", "The installed adapter configuration is unavailable."},
            });
        } else {
            candidate["configuration_state"] = "configured";
            try {
                configuration = read_json_file(configuration_path);
                configuration_ready = true;
            } catch (const std::exception&) {
                issues.push_back({
                    {"code", "bot_configuration_invalid"},
                    {"detail", "The installed adapter configuration is invalid."},
                });
            }
        }
        startup_installed =
            adapter.receipt.value("startup", json::object())
                .value("installed", false);
        candidate["startup_state"] =
            startup_installed ? "installed" : "disabled";
    } catch (const std::exception& error) {
        if (fs::is_regular_file(receipts_root / "lichess.adapter.json")) {
            candidate["package_state"] = "broken";
            candidate["integration_state"] = "incompatible";
            issues.push_back({
                {"code", "bot_adapter_broken"},
                {"detail", error.what()},
            });
        }
    }
    project_bot_prerequisite(
        readiness,
        issues,
        "verified_adapter",
        adapter_ready,
        "bot_adapter_unavailable",
        "Install or repair the verified Lichess Bot adapter.");
    readiness["startup"] = startup_installed ? "enabled" : "disabled";

    const bool runtime_ready = adapter_ready;
    if (!runtime_ready) {
        issues.push_back({
            {"code", "bot_runtime_unavailable"},
            {"detail", "The adapter's declared execution runtime is unavailable."},
        });
    }
    readiness["adapter_runtime"] = runtime_ready ? "ready" : "missing";

    bool engine_ready = false;
    bool model_ready = false;
    try {
        const InstalledComponent engine =
            installed_component(receipts_root, "uci_arena.engine");
        static_cast<void>(verified_entrypoint(engine, "uci"));
        engine_ready = true;
        if (engine.manifest.contains("default_model") &&
            engine.manifest.at("default_model").is_object()) {
            const json& model = engine.manifest.at("default_model");
            if (!model.value("model_id", "").empty() &&
                !model.value("display_name", "").empty() &&
                !model.value("input_adapter_id", "").empty()) {
                model_ready = true;
                candidate["default_model"] = model;
            }
        }
    } catch (const std::exception&) {
    }
    project_bot_prerequisite(
        readiness,
        issues,
        "verified_uci_arena_engine",
        engine_ready,
        "bot_engine_unavailable",
        "Install or repair the verified UCI Arena Engine component.");
    project_bot_prerequisite(
        readiness,
        issues,
        "active_model",
        model_ready,
        "bot_model_unavailable",
        "The installed UCI Arena Engine does not expose a verified default model.");

    bool opening_book_ready = false;
    try {
        static_cast<void>(
            installed_component(receipts_root, "opening_book.standard"));
        opening_book_ready = true;
    } catch (const std::exception&) {
    }
    project_bot_prerequisite(
        readiness,
        issues,
        "opening_book",
        opening_book_ready,
        "bot_opening_book_unavailable",
        "Install or repair the signed standard opening-book component.");

    bool syzygy_ready = false;
    try {
        static_cast<void>(
            installed_component(receipts_root, "syzygy.wdl.3-4-5"));
        syzygy_ready = true;
    } catch (const std::exception&) {
        syzygy_ready =
            saved_locator_reference(
                manager_data_root, "syzygy", true).has_value();
    }
    project_bot_prerequisite(
        readiness,
        issues,
        "syzygy",
        syzygy_ready,
        "bot_syzygy_unavailable",
        "Install bounded Syzygy WDL data or select a verified local Syzygy directory.");

    const auto timing =
        saved_locator_reference(manager_data_root, "timing_profile");
    const auto single_game =
        saved_locator_reference(
            manager_data_root, "lichess_single_game_profile");
    const auto capacity =
        saved_locator_reference(
            manager_data_root, "lichess_capacity_profile");
    const auto transport =
        saved_locator_reference(
            manager_data_root, "lichess_transport_profile");
    const auto configured_path_matches = [&configuration, configuration_ready](
                                             const std::vector<std::string>& fields,
                                             const std::optional<fs::path>& locator) {
        if (!configuration_ready || !locator.has_value()) {
            return false;
        }
        const json* current = &configuration;
        for (const auto& field : fields) {
            if (!current->is_object() || !current->contains(field)) {
                return false;
            }
            current = &current->at(field);
        }
        if (!current->is_string()) {
            return false;
        }
        return fs::absolute(current->get<std::string>()).lexically_normal() ==
               fs::absolute(*locator).lexically_normal();
    };
    const bool timing_ready = configured_path_matches(
        {"engine", "time_profile"}, timing);
    project_bot_prerequisite(
        readiness,
        issues,
        "timing_profile",
        timing_ready,
        "bot_timing_profile_unavailable",
        "Import a provider-safety-qualified timing profile whose semantic timing identity "
        "matches the configured model, provider/runtime, hardware, and search topology.");
    const bool single_game_ready = configured_path_matches(
        {"engine", "single_game_profile"}, single_game);
    project_bot_prerequisite(
        readiness,
        issues,
        "single_game_profile",
        single_game_ready,
        "bot_single_game_profile_unavailable",
        "Import an exact qualified Lichess single-game profile.");
    const bool capacity_configured = configured_path_matches(
        {"capacity", "capacity_profile"}, capacity);
    readiness["capacity_profile"] =
        capacity_configured ? "configured" : "missing";
    if (!capacity_configured) {
        issues.push_back({
            {"code", "bot_capacity_profile_unavailable"},
            {"detail", "Import a qualified Lichess capacity profile for this runtime."},
        });
    }
    const bool transport_ready = configured_path_matches(
        {"timing", "transport_profile_path"}, transport);
    readiness["transport_profile"] =
        transport_ready ? "configured" : "bootstrap";

    const auto token_reference = saved_locator_reference(
        manager_data_root, "lichess_token_path");
    const bool token_inside_adapter =
        token_reference.has_value() && adapter_root.has_value() &&
        same_or_within(*adapter_root, *token_reference);
    const bool token_ready =
        token_reference.has_value() && !token_inside_adapter;
    if (token_ready) {
        candidate["token_reference_available"] = true;
    }
    project_bot_prerequisite(
        readiness,
        issues,
        "token_reference",
        token_ready,
        token_inside_adapter
            ? "bot_token_reference_not_external"
            : "bot_token_reference_missing",
        token_inside_adapter
            ? "Move the Lichess token file outside the installed adapter and select it again."
            : "Select a Lichess token-file reference before probing the account.");
    readiness["bot_account_probe"] = "not_verified";
    candidate["readiness"] = readiness;
    candidate["issues"] = issues;
    const bool probe_ready =
        adapter_ready && runtime_ready && configuration_ready && token_ready;
    candidate["state"] = probe_ready ? "ready_to_probe" : "blocked";
    candidate["startup_eligibility"] =
        bot_live_prerequisites_ready(readiness) ? "ready" : "blocked";
    return {
        {"schema", "arena_bot_candidates_v1"},
        {"schema_version", 1},
        {"candidates", json::array({std::move(candidate)})},
    };
}

json ServiceCatalog::probe_bot(
    const fs::path& manager_data_root,
    const ProcessSupervisor& process_supervisor) {
    return probe_bot(
        manager_data_root,
        [&process_supervisor](
            const fs::path& executable,
            const fs::path& working_directory,
            const std::vector<std::string>& arguments,
            const std::chrono::milliseconds timeout) {
            return process_supervisor.run_json(
                executable, working_directory, arguments, timeout);
        });
}

json ServiceCatalog::probe_bot(
    const fs::path& manager_data_root,
    const BotCommandRunner& command_runner) {
    const BotProviderContext provider =
        bot_provider_context(manager_data_root);
    const fs::path token_path =
        saved_locator_reference(manager_data_root, "lichess_token_path")
            .value_or(fs::path{});
    if (token_path.empty()) {
        throw std::runtime_error(
            "a valid Lichess token-file reference is required");
    }
    if (same_or_within(provider.adapter.active_root, token_path)) {
        throw std::runtime_error(
            "the Lichess token file must be outside the installed adapter");
    }
    std::vector<std::string> arguments = provider_operation_arguments(
        provider.contract, "account_probe", "external_file_path_only");
    arguments.insert(
        arguments.end(),
        {
            "--config",
            provider.configuration_path.string(),
            "--token-file",
            token_path.string(),
        });
    const json result = command_runner(
        provider.contract.executable,
        provider.adapter.active_root,
        arguments,
        std::chrono::seconds(30));
    if (result.value("status", "") != "ready" ||
        !result.value("is_bot", false) ||
        !result.contains("account") ||
        !result.at("account").is_string() ||
        !valid_lichess_account_name(
            result.at("account").get_ref<const std::string&>()) ||
        (result.contains("games") &&
         !result.at("games").is_number_unsigned())) {
        throw std::runtime_error(
            "the adapter did not return a valid BOT account identity");
    }
    return {
        {"schema", "arena_bot_probe_result_v1"},
        {"schema_version", 1},
        {"state", "passed"},
        {"account_name", result.at("account")},
        {"is_bot", true},
        {"games", result.value("games", 0ULL)},
    };
}

json ServiceCatalog::qualify_bot(
    const fs::path& manager_data_root,
    const ProcessSupervisor& process_supervisor) {
    return qualify_bot(
        manager_data_root,
        [&process_supervisor](
            const fs::path& executable,
            const fs::path& working_directory,
            const std::vector<std::string>& arguments,
            const std::chrono::milliseconds timeout) {
            return process_supervisor.run_json(
                executable, working_directory, arguments, timeout);
        });
}

json ServiceCatalog::discover_bot(
    const fs::path& manager_data_root,
    const ProcessSupervisor& process_supervisor) {
    return discover_bot(
        manager_data_root,
        [&process_supervisor](
            const fs::path& executable,
            const fs::path& working_directory,
            const std::vector<std::string>& arguments,
            const std::chrono::milliseconds timeout) {
            return process_supervisor.run_json(
                executable, working_directory, arguments, timeout);
        });
}

json ServiceCatalog::discover_bot(
    const fs::path& manager_data_root,
    const BotCommandRunner& command_runner) {
    json discovery = discover_bot(manager_data_root);
    if (!discovery.contains("candidates") ||
        !discovery.at("candidates").is_array() ||
        discovery.at("candidates").empty()) {
        return discovery;
    }
    json& candidate = discovery.at("candidates").at(0);
    json& readiness = candidate.at("readiness");
    if (readiness.value("capacity_profile", "missing") != "configured") {
        return discovery;
    }

    try {
        const json options = bot_options(manager_data_root, command_runner);
        const json& evidence =
            options.at("capacity").at("evidence");
        if (evidence.at("qualified").get<bool>()) {
            readiness["capacity_profile"] = "ready";
        } else {
            candidate.at("issues").push_back({
                {"code", "bot_capacity_profile_unqualified"},
                {"detail",
                 "The Lichess provider rejected the configured capacity evidence. "
                 "Requalify the installed engine before enabling startup."},
            });
        }
    } catch (const std::exception&) {
        candidate.at("issues").push_back({
            {"code", "bot_capacity_profile_status_unavailable"},
            {"detail",
             "The Lichess provider could not verify the configured capacity evidence. "
             "Refresh provider options or requalify the installed engine."},
        });
    }
    candidate["startup_eligibility"] =
        bot_live_prerequisites_ready(readiness) ? "ready" : "blocked";
    return discovery;
}

json ServiceCatalog::qualify_bot(
    const fs::path& manager_data_root,
    const BotCommandRunner& command_runner) {
    const BotProviderContext provider = bot_provider_context(manager_data_root);
    const fs::path runtime_directory = bot_runtime_directory(provider.adapter);
    std::vector<std::string> arguments = provider_operation_arguments(
        provider.contract, "qualification_run", "forbidden");
    arguments.insert(
        arguments.end(),
        {
            "--config",
            provider.configuration_path.string(),
            "--runtime-dir",
            runtime_directory.string(),
        });
    const json result = command_runner(
        provider.contract.executable,
        provider.adapter.active_root,
        arguments,
        std::chrono::minutes(20));

    if (result.is_object() && result.size() == 3U &&
        result.value("schema", "") ==
            "uci_arena_lichess_qualification_result_v1" &&
        result.value("status", "") == "failed" &&
        result.contains("failure") && result.at("failure").is_object()) {
        const json& failure = result.at("failure");
        const std::string code = failure.value("code", "");
        const std::string stage = failure.value("stage", "");
        const std::string detail = failure.value("detail", "");
        const std::set<std::string> stages{
            "startup_safety",
            "preflight",
            "complete_game",
            "single_game_publication",
            "capacity_probe",
            "commit",
        };
        if (failure.size() != 3U || !valid_contract_identifier(code) ||
            !stages.contains(stage) ||
            code != "qualification_" + stage + "_failed" ||
            !valid_qualification_failure_detail(detail)) {
            throw std::runtime_error(
                "the provider returned an invalid qualification failure");
        }
        throw BotQualificationFailure(code, stage, detail);
    }
    if (!result.is_object() || result.dump().size() > 16U * 1024U ||
        result.size() > 32U ||
        result.value("schema", "") !=
            "uci_arena_lichess_qualification_result_v1" ||
        result.value("status", "") != "qualified" ||
        !result.contains("reused") || !result.at("reused").is_boolean() ||
        !result.contains("maximum_concurrency") ||
        !result.at("maximum_concurrency").is_number_integer()) {
        throw std::runtime_error(
            "the provider returned invalid installed qualification state");
    }
    const auto maximum_concurrency =
        result.at("maximum_concurrency").get<std::int64_t>();
    const std::string search_core = result.value("search_core", "");
    const std::string provider_name = result.value("provider", "");
    const std::string model_id = result.value("model_id", "");
    const std::string completed_at = result.value("completed_at", "");
    if (maximum_concurrency < 1 || maximum_concurrency > 64 ||
        (search_core != "mcgs" && search_core != "v2") ||
        (provider_name != "directml" && provider_name != "cuda") ||
        !valid_contract_identifier(model_id) || completed_at.empty() ||
        completed_at.size() > 64U) {
        throw std::runtime_error(
            "the provider returned invalid installed qualification identity");
    }
    for (const char* field : {
             "engine_sha256",
             "timing_sha256",
             "timing_identity_sha256",
             "single_game_profile_sha256",
             "capacity_profile_sha256",
         }) {
        if (!result.contains(field) || !result.at(field).is_string() ||
            !valid_prefixed_sha256(
                result.at(field).get_ref<const std::string&>())) {
            throw std::runtime_error(
                "the provider returned an invalid qualification digest");
        }
    }

    const auto verified_output = [&runtime_directory, &result](
                                     const char* field,
                                     const char* digest_field,
                                     const char* expected_filename) {
        if (!result.contains(field) || !result.at(field).is_string()) {
            throw std::runtime_error(
                "the provider omitted an installed qualification output");
        }
        const fs::path declared = result.at(field).get<std::string>();
        std::error_code error;
        const fs::path canonical_root =
            fs::weakly_canonical(runtime_directory, error);
        if (error) {
            throw std::runtime_error(
                "the provider runtime directory cannot be verified");
        }
        const fs::path canonical_output = fs::weakly_canonical(declared, error);
        if (error || !declared.is_absolute() ||
            declared.filename() != expected_filename ||
            !fs::is_regular_file(declared) || fs::is_symlink(declared) ||
            !same_or_within(canonical_root, canonical_output)) {
            throw std::runtime_error(
                "the provider qualification output escaped its retained workspace");
        }
        const std::string observed = "sha256:" + sha256_file(canonical_output);
        if (observed != result.at(digest_field).get<std::string>()) {
            throw std::runtime_error(
                "the provider qualification output digest does not match");
        }
        return canonical_output;
    };
    const fs::path single_game = verified_output(
        "single_game_profile",
        "single_game_profile_sha256",
        "uci_arena_single_game_profile_v1.json");
    const fs::path capacity = verified_output(
        "capacity_profile",
        "capacity_profile_sha256",
        "uci_arena_lichess_capacity_profile_v2.json");
    return {
        {"schema", "arena_bot_qualification_internal_v1"},
        {"single_game_profile", single_game.string()},
        {"capacity_profile", capacity.string()},
        {"reused", result.at("reused")},
        {"maximum_concurrency", maximum_concurrency},
        {"search_core", search_core},
        {"model_id", model_id},
        {"provider", provider_name},
        {"timing_identity_sha256", result.at("timing_identity_sha256")},
    };
}

json ServiceCatalog::control_bot(
    const fs::path& manager_data_root,
    const ProcessSupervisor& process_supervisor,
    const std::string& action) {
    return control_bot(
        manager_data_root,
        [&process_supervisor](
            const fs::path& executable,
            const fs::path& working_directory,
            const std::vector<std::string>& arguments,
            const std::chrono::milliseconds timeout) {
            return process_supervisor.run_json(
                executable, working_directory, arguments, timeout);
        },
        action);
}

json ServiceCatalog::control_bot(
    const fs::path& manager_data_root,
    const BotCommandRunner& command_runner,
    const std::string& action) {
    if (action != "status" && action != "enable_startup" &&
        action != "recover_startup" && action != "disable_startup") {
        throw std::invalid_argument("unsupported BOT account-control action");
    }
    const BotProviderContext provider =
        bot_provider_context(manager_data_root);
    const fs::path runtime_directory =
        bot_runtime_directory(provider.adapter);
    std::optional<std::string> account_name;
    const std::string provider_operation =
        action == "enable_startup" ? "startup_install"
        : action == "recover_startup" ? "startup_recover"
        : action == "disable_startup" ? "startup_uninstall"
                                      : "startup_status";
    std::vector<std::string> arguments = provider_operation_arguments(
        provider.contract,
        provider_operation,
        action == "enable_startup" ? "external_file_path_only" : "forbidden");
    if (action == "enable_startup") {
        if (discover_bot(manager_data_root, command_runner)
                .at("candidates")
                .at(0)
                .at("startup_eligibility") != "ready") {
            throw std::runtime_error(
                "every BOT live-play readiness prerequisite is required before startup");
        }
        const json probe = probe_bot(manager_data_root, command_runner);
        account_name = probe.at("account_name").get<std::string>();
        const std::string configuration_path =
            provider.adapter.receipt.value("configuration_path", "");
        const fs::path token_path =
            saved_locator_reference(manager_data_root, "lichess_token_path")
                .value_or(fs::path{});
        if (configuration_path.empty() || token_path.empty()) {
            throw std::runtime_error(
                "BOT startup configuration is incomplete");
        }
        arguments.insert(
            arguments.end(),
            {
                "--config",
                configuration_path,
                "--token-file",
                token_path.string(),
                "--expected-account",
                *account_name,
                "--runtime-dir",
                runtime_directory.string(),
            });
    } else if (action == "recover_startup" || action == "disable_startup") {
        arguments.insert(
            arguments.end(),
            {"--runtime-dir", runtime_directory.string()});
    }

    const json result = command_runner(
        provider.contract.executable,
        provider.adapter.active_root,
        arguments,
        action == "enable_startup"
            ? std::chrono::minutes(5)
            : std::chrono::seconds(30));
    if (result.value("status", "") != "ready" ||
        !result.contains("startup") ||
        !result.at("startup").is_object() ||
        !result.at("startup").contains("installed") ||
        !result.at("startup").at("installed").is_boolean()) {
        throw std::runtime_error(
            "the provider returned invalid BOT startup status");
    }
    const json& startup = result.at("startup");
    const bool installed = startup.at("installed").get<bool>();
    if (action == "enable_startup" && !installed) {
        throw std::runtime_error(
            "the provider did not install BOT startup");
    }
    if (action == "disable_startup" && installed) {
        throw std::runtime_error(
            "the provider did not remove BOT startup");
    }
    if (action == "recover_startup" &&
        (!installed || startup.value("recoveryRequested", false) != true ||
         startup.value("registrationMutated", true) != false)) {
        throw std::runtime_error(
            "the provider returned invalid BOT startup recovery state");
    }
    std::vector<std::string> status_arguments = provider_operation_arguments(
        provider.contract, "provider_status", "forbidden");
    status_arguments.insert(
        status_arguments.end(),
        {"--runtime-dir", runtime_directory.string()});
    json provider_status;
    try {
        provider_status = project_bot_provider_status(command_runner(
            provider.contract.executable,
            provider.adapter.active_root,
            status_arguments,
            std::chrono::seconds(30)));
    } catch (const std::exception&) {
        provider_status = {
            {"provider_state", "unavailable"},
            {"provider_active", false},
            {"provider_ready", false},
            {"stream_connected", false},
            {"blockers", json::array({"provider_status_unavailable"})},
        };
    }
    const bool active = provider_status.at("provider_active").get<bool>();
    json response{
        {"schema", "arena_bot_control_result_v1"},
        {"schema_version", 1},
        {"state", !installed ? "disabled" : active ? "active" : "enabled"},
        {"installed", installed},
        {"active", active},
    };
    response.update(provider_status);
    if (account_name.has_value()) {
        response["account_name"] = *account_name;
    }
    if (action == "recover_startup") {
        response["recovery_requested"] = true;
    }
    return response;
}

json ServiceCatalog::bot_options(
    const fs::path& manager_data_root,
    const ProcessSupervisor& process_supervisor) {
    return bot_options(
        manager_data_root,
        [&process_supervisor](
            const fs::path& executable,
            const fs::path& working_directory,
            const std::vector<std::string>& arguments,
            const std::chrono::milliseconds timeout) {
            return process_supervisor.run_json(
                executable, working_directory, arguments, timeout);
        });
}

json ServiceCatalog::bot_options(
    const fs::path& manager_data_root,
    const BotCommandRunner& command_runner) {
    const BotProviderContext provider =
        bot_provider_context(manager_data_root);
    std::vector<std::string> arguments = provider_operation_arguments(
        provider.contract, "config_get", "forbidden");
    arguments.insert(
        arguments.end(),
        {"--config", provider.configuration_path.string()});
    const json result = command_runner(
        provider.contract.executable,
        provider.adapter.active_root,
        arguments,
        std::chrono::seconds(30));
    return project_bot_options(result);
}

json ServiceCatalog::update_bot_options(
    const fs::path& manager_data_root,
    const ProcessSupervisor& process_supervisor,
    const json& settings,
    const bool confirm_rated,
    const bool confirm_outbound) {
    return update_bot_options(
        manager_data_root,
        [&process_supervisor](
            const fs::path& executable,
            const fs::path& working_directory,
            const std::vector<std::string>& arguments,
            const std::chrono::milliseconds timeout) {
            return process_supervisor.run_json(
                executable, working_directory, arguments, timeout);
        },
        settings,
        confirm_rated,
        confirm_outbound);
}

json ServiceCatalog::update_bot_options(
    const fs::path& manager_data_root,
    const BotCommandRunner& command_runner,
    const json& settings,
    const bool confirm_rated,
    const bool confirm_outbound) {
    if (!settings.is_object() || settings.empty() ||
        settings.dump().size() > 32U * 1024U) {
        throw std::invalid_argument(
            "BOT option settings must be a non-empty bounded object");
    }
    static const std::set<std::string> allowed_groups{
        "capacity",
        "inbound_challenges",
        "outbound_challenge",
        "scheduler",
        "opponent_selection",
    };
    for (const auto& [group, value] : settings.items()) {
        if (!allowed_groups.contains(group) || !value.is_object()) {
            throw std::invalid_argument(
                "BOT option settings contain an unsupported group");
        }
    }
    const BotProviderContext provider =
        bot_provider_context(manager_data_root);
    std::vector<std::string> arguments = provider_operation_arguments(
        provider.contract, "config_update", "forbidden");
    arguments.insert(
        arguments.end(),
        {
            "--config",
            provider.configuration_path.string(),
            "--settings",
            settings.dump(),
        });
    if (confirm_rated) {
        arguments.insert(
            arguments.end(),
            {"--confirm-rated", "I_ACCEPT_RATED_BOT_GAMES"});
    }
    if (confirm_outbound) {
        arguments.insert(
            arguments.end(),
            {"--confirm-outbound",
             "I_ACCEPT_AUTOMATIC_OUTBOUND_CHALLENGES"});
    }
    const json result = command_runner(
        provider.contract.executable,
        provider.adapter.active_root,
        arguments,
        std::chrono::seconds(30));
    return project_bot_options(result);
}

json ServiceCatalog::control_bot_account(
    const fs::path& manager_data_root,
    const ProcessSupervisor& process_supervisor,
    const std::string& action,
    const json& parameters) {
    return control_bot_account(
        manager_data_root,
        [&process_supervisor](
            const fs::path& executable,
            const fs::path& working_directory,
            const std::vector<std::string>& arguments,
            const std::chrono::milliseconds timeout) {
            return process_supervisor.run_json(
                executable, working_directory, arguments, timeout);
        },
        action,
        parameters);
}

json ServiceCatalog::control_bot_account(
    const fs::path& manager_data_root,
    const BotCommandRunner& command_runner,
    const std::string& action,
    const json& parameters) {
    static const std::set<std::string> actions{
        "owner_list", "owner_add", "owner_remove",
        "reservation_status", "reservation_create", "reservation_cancel",
        "scheduler_status", "scheduler_pause", "scheduler_resume",
    };
    if (!actions.contains(action)) {
        throw std::invalid_argument(
            "unsupported BOT local account-control action");
    }
    std::vector<std::string> arguments;
    if (action == "owner_list") {
        require_parameter_keys(parameters, {});
    } else if (action == "owner_add" || action == "owner_remove") {
        require_parameter_keys(parameters, {"username"});
        arguments = {
            "--username",
            account_parameter(parameters, "username"),
        };
    } else if (action == "reservation_status" ||
               action == "reservation_cancel") {
        require_parameter_keys(parameters, {});
    } else if (action == "reservation_create") {
        require_parameter_keys(
            parameters, {"opponent", "games", "minutes"});
        arguments = {
            "--opponent",
            account_parameter(parameters, "opponent"),
            "--games",
            std::to_string(
                bounded_integer_parameter(parameters, "games", 1, 100)),
            "--minutes",
            std::to_string(
                bounded_integer_parameter(parameters, "minutes", 1, 1440)),
        };
    } else {
        require_parameter_keys(parameters, {});
    }

    const BotProviderContext provider =
        bot_provider_context(manager_data_root);
    std::vector<std::string> invocation = provider_operation_arguments(
        provider.contract, action, "forbidden");
    invocation.insert(invocation.end(), arguments.begin(), arguments.end());
    arguments = std::move(invocation);
    arguments.insert(
        arguments.end(),
        {"--runtime-dir", bot_runtime_directory(provider.adapter).string()});
    const json result = command_runner(
        provider.contract.executable,
        provider.adapter.active_root,
        arguments,
        std::chrono::seconds(30));
    if (!result.is_object() || result.dump().size() > 32U * 1024U ||
        result.value("status", "") != "ready") {
        throw std::runtime_error(
            "the provider returned invalid BOT local account state");
    }
    json response{
        {"schema", "arena_bot_local_account_result_v1"},
        {"schema_version", 1},
        {"action", action},
    };
    if (action == "owner_list") {
        if (!result.contains("owners") ||
            !result.at("owners").is_array() ||
            result.at("owners").size() > 128U) {
            throw std::runtime_error("the provider returned invalid owners");
        }
        response["owners"] = json::array();
        for (const auto& owner : result.at("owners")) {
            if (!owner.is_object() ||
                !valid_lichess_account_name(owner.value("username", "")) ||
                !owner.contains("created_at") ||
                !owner.at("created_at").is_number_integer()) {
                throw std::runtime_error(
                    "the provider returned invalid owners");
            }
            response["owners"].push_back({
                {"username", owner.at("username")},
                {"created_at", owner.at("created_at")},
            });
        }
    } else if (action == "owner_add" || action == "owner_remove") {
        const std::string username = result.value("username", "");
        const std::string outcome =
            action == "owner_add" ? "added" : "removed";
        if (!valid_lichess_account_name(username) ||
            !result.contains(outcome) || !result.at(outcome).is_boolean()) {
            throw std::runtime_error(
                "the provider returned invalid owner state");
        }
        response["username"] = username;
        response[outcome] = result.at(outcome);
    } else if (action.starts_with("reservation_")) {
        if (action == "reservation_cancel") {
            if (!result.contains("canceled") ||
                !result.at("canceled").is_boolean()) {
                throw std::runtime_error(
                    "the provider returned invalid reservation state");
            }
            response["canceled"] = result.at("canceled");
        } else if (!result.contains("reservation") ||
                   !(result.at("reservation").is_null() ||
                     result.at("reservation").is_object())) {
            throw std::runtime_error(
                "the provider returned invalid reservation state");
        } else if (result.at("reservation").is_null()) {
            response["reservation"] = nullptr;
        } else {
            const json& reservation = result.at("reservation");
            if (!valid_lichess_account_name(
                    reservation.value("opponent", "")) ||
                !reservation.contains("games_remaining") ||
                !reservation.at("games_remaining").is_number_integer() ||
                !reservation.contains("expires_at") ||
                !reservation.at("expires_at").is_number_integer() ||
                !reservation.contains("created_at") ||
                !reservation.at("created_at").is_number_integer()) {
                throw std::runtime_error(
                    "the provider returned invalid reservation state");
            }
            response["reservation"] = selected_fields(
                reservation,
                {"opponent", "games_remaining", "expires_at", "created_at"});
        }
    } else {
        if (!result.contains("scheduler") ||
            !result.at("scheduler").is_object() ||
            !result.at("scheduler").contains("paused") ||
            !result.at("scheduler").at("paused").is_boolean()) {
            throw std::runtime_error(
                "the provider returned invalid scheduler state");
        }
        response["scheduler"] = {
            {"paused", result.at("scheduler").at("paused")},
        };
    }
    return response;
}

}  // namespace arena::application
