#include "arena/application/component_verification_registry.hpp"

#include "arena/platform/path_inspection.hpp"
#include "arena/ports/component_change_monitor.hpp"

#include <sodium.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <optional>
#include <set>
#include <string_view>
#include <thread>
#include <tuple>
#include <unordered_map>
#include <utility>

namespace arena::application {
namespace {

using nlohmann::json;
namespace fs = std::filesystem;

constexpr std::uintmax_t max_metadata_bytes = 1024U * 1024U;
constexpr std::size_t max_manifest_files = 65'536U;
constexpr std::size_t max_receipts = 1024U;

constexpr std::array known_components{
    std::tuple{"uci_arena.engine", "UCI Arena Engine", false},
    std::tuple{"stockfish.engine", "Stockfish", false},
    std::tuple{"node_runtime.private", "Private Node runtime", false},
    std::tuple{"book_forge.service", "Book Forge", false},
    std::tuple{"opening_book.standard", "Opening book", false},
    std::tuple{"syzygy.wdl.3-4-5", "Syzygy WDL 3-4-5", true},
    std::tuple{"uci_arena_evidence.service", "UCI Arena Evidence Service", false},
    std::tuple{"lichess.adapter", "Lichess Bot adapter", true},
};

[[noreturn]] void verification_failure(const std::string& code, const std::string& detail) {
    throw ComponentVerificationUnavailable(code, detail);
}

json read_json_bounded(const fs::path& path, const std::string& code) {
    const auto observation = platform::inspect_path_no_follow(path);
    if (observation.kind != platform::PathEntryKind::regular_file ||
        observation.size_bytes > max_metadata_bytes) {
        verification_failure(code, "Installed component metadata is unavailable.");
    }
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        verification_failure(code, "Installed component metadata is unavailable.");
    }
    try {
        return json::parse(input);
    } catch (const json::exception&) {
        verification_failure(code, "Installed component metadata is malformed.");
    }
}

bool safe_relative_path(const fs::path& path) {
    if (path.empty() || path.is_absolute() || path.has_root_name()) {
        return false;
    }
    return std::ranges::none_of(path,
                                [](const fs::path& part) { return part == "." || part == ".."; });
}

bool valid_component_id(const std::string_view value) {
    return !value.empty() && value.size() <= 128U &&
           std::ranges::all_of(value, [](const unsigned char character) {
               return std::isalnum(character) != 0 || character == '.' || character == '_' ||
                      character == '-';
           });
}

bool valid_sha256(const std::string& value) {
    return value.size() == crypto_hash_sha256_BYTES * 2U &&
           std::ranges::all_of(value, [](const unsigned char character) {
               return std::isdigit(character) != 0 || (character >= 'a' && character <= 'f');
           });
}

std::string sha256_file(const fs::path& path) {
    if (sodium_init() < 0) {
        verification_failure("component_verification_unavailable",
                             "Cryptographic component verification is unavailable.");
    }
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        verification_failure("component_file_unavailable",
                             "An installed component file is unavailable.");
    }
    crypto_hash_sha256_state state{};
    crypto_hash_sha256_init(&state);
    std::array<unsigned char, 64U * 1024U> buffer{};
    while (input) {
        input.read(reinterpret_cast<char*>(buffer.data()), buffer.size());
        const auto count = input.gcount();
        if (count > 0) {
            crypto_hash_sha256_update(&state, buffer.data(),
                                      static_cast<unsigned long long>(count));
        }
    }
    if (!input.eof()) {
        verification_failure("component_file_unavailable",
                             "An installed component file could not be read completely.");
    }
    std::array<unsigned char, crypto_hash_sha256_BYTES> digest{};
    crypto_hash_sha256_final(&state, digest.data());
    std::array<char, crypto_hash_sha256_BYTES * 2U + 1U> encoded{};
    sodium_bin2hex(encoded.data(), encoded.size(), digest.data(), digest.size());
    return encoded.data();
}

VerifiedComponent verify_installed_component(const fs::path& receipts_root,
                                             const std::string& component_id,
                                             const bool allow_disabled) {
    if (!valid_component_id(component_id)) {
        verification_failure("component_identity_invalid",
                             "The installed component identity is invalid.");
    }
    if (platform::inspect_path_no_follow(receipts_root).kind !=
        platform::PathEntryKind::directory) {
        verification_failure("component_receipt_unavailable",
                             "Installed component metadata is unavailable.");
    }
    const fs::path receipt_path = receipts_root / (component_id + ".json");
    const auto receipt_observation = platform::inspect_path_no_follow(receipt_path);
    if (receipt_observation.kind == platform::PathEntryKind::missing) {
        verification_failure("component_not_installed", "The component is not installed.");
    }
    const json receipt = read_json_bounded(receipt_path, "component_receipt_unavailable");
    const std::string state = receipt.value("state", "");
    if (!receipt.is_object() || receipt.size() > 128U ||
        receipt.value("schema", "") != "arena_install_receipt_v1" ||
        receipt.value("schema_version", 1) != 1 ||
        receipt.value("component_id", "") != component_id ||
        (state != "installed_managed" && !(allow_disabled && state == "installed_disabled")) ||
        !receipt.contains("active_root") || !receipt.at("active_root").is_string()) {
        verification_failure("component_receipt_invalid",
                             "The installed component receipt is invalid.");
    }
    const fs::path declared_root = receipt.at("active_root").get<std::string>();
    if (!declared_root.is_absolute()) {
        verification_failure("component_root_invalid", "The installed component root is invalid.");
    }
    const fs::path active_root = declared_root.lexically_normal();
    if (platform::inspect_path_no_follow(active_root).kind != platform::PathEntryKind::directory) {
        verification_failure("component_root_unavailable",
                             "The installed component root is unavailable.");
    }
    const json manifest =
        read_json_bounded(active_root / "arena-component.json", "component_manifest_unavailable");
    if (!manifest.is_object() || manifest.size() > 128U ||
        manifest.value("schema", "") != "arena_install_component_v1" ||
        manifest.value("schema_version", 1) != 1 ||
        manifest.value("component_id", "") != component_id ||
        manifest.value("component_version", "") != receipt.value("component_version", "") ||
        !manifest.contains("files") || !manifest.at("files").is_array() ||
        manifest.at("files").size() > max_manifest_files) {
        verification_failure("component_manifest_invalid",
                             "The component manifest does not match its receipt.");
    }

    std::set<fs::path> listed;
    for (const auto& file : manifest.at("files")) {
        if (!file.is_object() || file.size() > 16U || !file.contains("path") ||
            !file.at("path").is_string() || !file.contains("size_bytes") ||
            !file.at("size_bytes").is_number_unsigned() || !file.contains("sha256") ||
            !file.at("sha256").is_string()) {
            verification_failure("component_inventory_invalid",
                                 "The component file inventory is invalid.");
        }
        const fs::path relative = fs::path(file.at("path").get<std::string>()).lexically_normal();
        const std::string expected_sha256 = file.at("sha256").get<std::string>();
        if (!safe_relative_path(relative) || !listed.insert(relative).second ||
            !valid_sha256(expected_sha256)) {
            verification_failure("component_inventory_invalid",
                                 "The component file inventory is invalid.");
        }
        const fs::path path = active_root / relative;
        const auto expected_size = file.at("size_bytes").get<std::uintmax_t>();
        const auto observation = platform::inspect_path_no_follow(path);
        if (observation.kind != platform::PathEntryKind::regular_file ||
            observation.size_bytes != expected_size || sha256_file(path) != expected_sha256) {
            verification_failure("component_digest_mismatch",
                                 "An installed component file failed exact verification.");
        }
    }

    std::error_code error;
    fs::recursive_directory_iterator iterator(active_root, fs::directory_options::none, error);
    const fs::recursive_directory_iterator end;
    if (error) {
        verification_failure("component_tree_unavailable",
                             "The installed component tree cannot be inspected.");
    }
    while (iterator != end) {
        const auto observation = platform::inspect_path_no_follow(iterator->path());
        if (observation.kind == platform::PathEntryKind::link_or_reparse ||
            observation.kind == platform::PathEntryKind::other) {
            verification_failure("component_tree_unsafe",
                                 "The installed component tree contains an unsafe entry.");
        }
        if (observation.kind == platform::PathEntryKind::regular_file) {
            const fs::path relative =
                iterator->path().lexically_relative(active_root).lexically_normal();
            if (relative != fs::path("arena-component.json") && !listed.contains(relative)) {
                verification_failure("component_unlisted_file",
                                     "The installed component contains an unlisted file.");
            }
        } else if (observation.kind != platform::PathEntryKind::directory) {
            verification_failure("component_tree_unavailable",
                                 "The installed component tree cannot be inspected.");
        }
        iterator.increment(error);
        if (error) {
            verification_failure("component_tree_unavailable",
                                 "The installed component tree cannot be inspected.");
        }
    }
    return {receipt, manifest, active_root};
}

std::optional<fs::path> active_root_hint(const fs::path& receipts_root,
                                         const std::string& component_id) {
    try {
        const json receipt = read_json_bounded(receipts_root / (component_id + ".json"),
                                               "component_receipt_unavailable");
        if (!receipt.contains("active_root") || !receipt.at("active_root").is_string()) {
            return std::nullopt;
        }
        const fs::path root = receipt.at("active_root").get<std::string>();
        if (!root.is_absolute() ||
            platform::inspect_path_no_follow(root).kind != platform::PathEntryKind::directory) {
            return std::nullopt;
        }
        return root.lexically_normal();
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

std::set<std::string> retained_catalog_components(const fs::path& manager_data_root) {
    std::set<std::string> result;
    try {
        const json catalog = read_json_bounded(manager_data_root / "release" / "catalog.json",
                                               "retained_catalog_unavailable");
        if (catalog.value("schema", "") != "arena_install_catalog_v1" ||
            !catalog.contains("components") || !catalog.at("components").is_array() ||
            catalog.at("components").size() > 4096U) {
            return result;
        }
        for (const auto& component : catalog.at("components")) {
            const std::string id = component.value("component_id", "");
            if (valid_component_id(id)) {
                result.insert(id);
            }
        }
    } catch (const std::exception&) {
    }
    return result;
}

} // namespace

ComponentVerificationUnavailable::ComponentVerificationUnavailable(std::string code,
                                                                   std::string detail)
    : std::runtime_error(std::move(detail)), code_(std::move(code)) {}

const std::string& ComponentVerificationUnavailable::code() const noexcept {
    return code_;
}

struct ComponentVerificationRegistry::Impl final {
    enum class Phase { checking, verified, failed };

    struct Entry final {
        Phase phase{Phase::checking};
        bool receipt_present{false};
        bool queued{false};
        bool initial_complete{false};
        std::uint64_t generation{0};
        std::string issue_code{"component_verification_checking"};
        std::string issue_detail{"Component verification is in progress."};
        std::optional<VerifiedComponent> component;
    };

    explicit Impl(VerifyFunction supplied)
        : verifier(supplied ? std::move(supplied) : verify_installed_component) {
        for (const auto& [component_id, display_name, optional] : known_components) {
            static_cast<void>(display_name);
            static_cast<void>(optional);
            entries.emplace(component_id, Entry{});
        }
    }

    void enqueue_locked(const std::string& component_id) {
        Entry& entry = entries.at(component_id);
        if (!entry.queued) {
            entry.queued = true;
            queue.push_back(component_id);
        }
    }

    void invalidate_locked(const std::string& component_id, const std::string& code,
                           const std::string& detail) {
        Entry& entry = entries.at(component_id);
        ++entry.generation;
        entry.phase = Phase::checking;
        entry.issue_code = code;
        entry.issue_detail = detail;
        entry.component.reset();
        enqueue_locked(component_id);
    }

    void invalidate(const std::string& component_id,
                    const std::string& code = "component_verification_stale",
                    const std::string& detail =
                        "Component files changed and exact revalidation is in progress.") {
        std::lock_guard lock(mutex);
        if (!started || stopping || !entries.contains(component_id)) {
            return;
        }
        invalidate_locked(component_id, code, detail);
        condition.notify_all();
    }

    void invalidate_all() {
        std::lock_guard lock(mutex);
        if (!started || stopping) {
            return;
        }
        for (auto& [component_id, entry] : entries) {
            static_cast<void>(entry);
            invalidate_locked(
                component_id, "component_watcher_overflow",
                "Component change notifications overflowed; exact revalidation is in progress.");
        }
        condition.notify_all();
    }

    void on_data_change(const ports::ComponentChange& change) {
        if (change.kind == ports::ComponentChangeKind::overflow) {
            invalidate_all();
            return;
        }
        const fs::path relative = change.relative_path.lexically_normal();
        auto part = relative.begin();
        if (part == relative.end()) {
            return;
        }
        const std::string first = part->generic_string();
        ++part;
        if (first == "receipts" && part != relative.end()) {
            const fs::path filename = *part;
            if (filename.extension() == ".json") {
                const std::string component_id = filename.stem().string();
                if (valid_component_id(component_id)) {
                    {
                        std::lock_guard lock(mutex);
                        entries.try_emplace(component_id, Entry{});
                    }
                    invalidate(component_id);
                }
            }
            return;
        }
        if (first == "release") {
            std::lock_guard lock(mutex);
            downloadable = retained_catalog_components(data_root);
        }
    }

    void complete_initial_locked(Entry& entry) {
        if (!entry.initial_complete) {
            entry.initial_complete = true;
            ++initial_completed;
        }
    }

    void verify_one(const std::string& component_id) {
        std::uint64_t generation = 0;
        {
            std::lock_guard lock(mutex);
            Entry& entry = entries.at(component_id);
            entry.queued = false;
            generation = entry.generation;
        }

        ++attempts;
        const fs::path receipt_path = data_root / "receipts" / (component_id + ".json");
        std::error_code error;
        const bool receipt_present = fs::is_regular_file(receipt_path, error) && !error;
        std::unique_ptr<ports::ComponentChangeMonitor> next_monitor;
        if (receipt_present) {
            if (const auto root = active_root_hint(data_root / "receipts", component_id)) {
                next_monitor = ports::watch_component_tree(
                    *root, [this, component_id](const ports::ComponentChange& change) {
                        if (change.kind == ports::ComponentChangeKind::overflow) {
                            invalidate_all();
                        } else {
                            invalidate(component_id);
                        }
                    });
            }
        }
        const bool component_monitor_ready = !receipt_present || next_monitor != nullptr;

        std::unique_ptr<ports::ComponentChangeMonitor> previous_monitor;
        {
            std::lock_guard lock(mutex);
            auto monitor = component_monitors.find(component_id);
            if (monitor != component_monitors.end()) {
                previous_monitor = std::move(monitor->second);
                component_monitors.erase(monitor);
            }
            if (next_monitor) {
                component_monitors.emplace(component_id, std::move(next_monitor));
            }
        }
        previous_monitor.reset();

        std::optional<VerifiedComponent> verified;
        std::string failure_code;
        std::string failure_detail;
        if (!component_monitor_ready) {
            failure_code = "component_watcher_unavailable";
            failure_detail = "Filesystem change monitoring is unavailable for this component.";
        } else {
            try {
                verified = verifier(data_root / "receipts", component_id, true);
            } catch (const ComponentVerificationUnavailable& failure) {
                failure_code = failure.code();
                failure_detail = failure.what();
            } catch (const std::exception&) {
                failure_code = "component_verification_failed";
                failure_detail = "Exact component verification failed.";
            }
        }

        {
            std::lock_guard lock(mutex);
            Entry& entry = entries.at(component_id);
            if (entry.generation != generation) {
                enqueue_locked(component_id);
                condition.notify_all();
                return;
            }
            entry.receipt_present = receipt_present;
            if (verified) {
                entry.phase = Phase::verified;
                entry.issue_code.clear();
                entry.issue_detail.clear();
                entry.component = std::move(verified);
            } else if (failure_code == "component_not_installed") {
                entry.phase = Phase::verified;
                entry.receipt_present = false;
                entry.issue_code.clear();
                entry.issue_detail.clear();
                entry.component.reset();
            } else {
                entry.phase = Phase::failed;
                entry.issue_code =
                    failure_code.empty() ? "component_verification_failed" : failure_code;
                entry.issue_detail = failure_detail.empty() ? "Exact component verification failed."
                                                            : failure_detail.substr(0, 512U);
                entry.component.reset();
            }
            complete_initial_locked(entry);
            condition.notify_all();
        }
    }

    void run(const std::stop_token stop_token) {
        while (!stop_token.stop_requested()) {
            std::string component_id;
            {
                std::unique_lock lock(mutex);
                condition.wait(lock, stop_token, [this] { return stopping || !queue.empty(); });
                if (stopping || stop_token.stop_requested()) {
                    return;
                }
                component_id = std::move(queue.front());
                queue.pop_front();
            }
            verify_one(component_id);
        }
    }

    VerifyFunction verifier;
    mutable std::mutex mutex;
    mutable std::condition_variable_any condition;
    fs::path data_root;
    std::unordered_map<std::string, Entry> entries;
    std::set<std::string> downloadable;
    std::deque<std::string> queue;
    std::unique_ptr<ports::ComponentChangeMonitor> data_monitor;
    std::unordered_map<std::string, std::unique_ptr<ports::ComponentChangeMonitor>>
        component_monitors;
    std::jthread worker;
    std::atomic_size_t attempts{0};
    std::size_t initial_completed{0};
    bool started{false};
    bool stopping{false};
};

ComponentVerificationRegistry::ComponentVerificationRegistry(VerifyFunction verifier)
    : impl_(std::make_unique<Impl>(std::move(verifier))) {}

ComponentVerificationRegistry::~ComponentVerificationRegistry() {
    stop();
}

void ComponentVerificationRegistry::begin(const std::filesystem::path& manager_data_root) {
    const fs::path normalized = fs::absolute(manager_data_root).lexically_normal();
    {
        std::lock_guard lock(impl_->mutex);
        if (impl_->started) {
            if (impl_->data_root != normalized) {
                throw std::invalid_argument(
                    "component verification root differs from Manager startup authority");
            }
            return;
        }
        impl_->started = true;
        impl_->data_root = normalized;
        impl_->downloadable = retained_catalog_components(normalized);
        const fs::path receipts_root = normalized / "receipts";
        std::error_code error;
        if (platform::inspect_path_no_follow(receipts_root).kind ==
            platform::PathEntryKind::directory) {
            std::size_t admitted = 0;
            for (fs::directory_iterator
                     iterator(receipts_root, fs::directory_options::skip_permission_denied, error),
                 end;
                 iterator != end && admitted < max_receipts; iterator.increment(error)) {
                if (error) {
                    break;
                }
                if (platform::inspect_path_no_follow(iterator->path()).kind ==
                        platform::PathEntryKind::regular_file &&
                    iterator->path().extension() == ".json") {
                    const std::string component_id = iterator->path().stem().string();
                    if (valid_component_id(component_id)) {
                        impl_->entries.try_emplace(component_id, Impl::Entry{});
                        ++admitted;
                    }
                }
                error.clear();
            }
        }
        for (auto& [component_id, entry] : impl_->entries) {
            entry.phase = Impl::Phase::checking;
            entry.issue_code = "component_verification_checking";
            entry.issue_detail = "Component verification is in progress.";
            impl_->enqueue_locked(component_id);
        }
    }

    impl_->data_monitor = ports::watch_component_tree(
        normalized, [impl = impl_.get()](const ports::ComponentChange& change) {
            impl->on_data_change(change);
        });
    if (!impl_->data_monitor) {
        std::lock_guard lock(impl_->mutex);
        for (auto& [component_id, entry] : impl_->entries) {
            static_cast<void>(component_id);
            entry.phase = Impl::Phase::failed;
            entry.issue_code = "component_watcher_unavailable";
            entry.issue_detail = "Filesystem change monitoring is unavailable.";
            impl_->complete_initial_locked(entry);
        }
        impl_->queue.clear();
        impl_->condition.notify_all();
        return;
    }
    impl_->worker = std::jthread(
        [impl = impl_.get()](const std::stop_token stop_token) { impl->run(stop_token); });
    impl_->condition.notify_all();
}

void ComponentVerificationRegistry::stop() {
    if (!impl_) {
        return;
    }
    {
        std::lock_guard lock(impl_->mutex);
        if (!impl_->started || impl_->stopping) {
            return;
        }
        impl_->stopping = true;
    }
    impl_->condition.notify_all();
    if (impl_->worker.joinable()) {
        impl_->worker.request_stop();
        impl_->worker.join();
    }
    impl_->data_monitor.reset();
    impl_->component_monitors.clear();
}

bool ComponentVerificationRegistry::started() const {
    std::lock_guard lock(impl_->mutex);
    return impl_->started && !impl_->stopping;
}

bool ComponentVerificationRegistry::wait_for_initial_verification(
    const std::stop_token stop_token) const {
    std::unique_lock lock(impl_->mutex);
    if (!impl_->started) {
        return false;
    }
    return impl_->condition.wait(lock, stop_token, [this] {
        return impl_->stopping || impl_->initial_completed == impl_->entries.size();
    }) && !impl_->stopping;
}

bool ComponentVerificationRegistry::wait_for_current_verification(
    const std::stop_token stop_token) const {
    std::unique_lock lock(impl_->mutex);
    if (!impl_->started) {
        return false;
    }
    return impl_->condition.wait(lock, stop_token, [this] {
        return impl_->stopping || std::ranges::none_of(impl_->entries, [](const auto& value) {
                   return value.second.phase == Impl::Phase::checking;
               });
    }) && !impl_->stopping;
}

json ComponentVerificationRegistry::inventory() const {
    std::lock_guard lock(impl_->mutex);
    json components = json::array();
    const bool any_checking = std::ranges::any_of(impl_->entries, [](const auto& value) {
        return value.second.phase == Impl::Phase::checking;
    });
    const bool any_failed = std::ranges::any_of(impl_->entries, [](const auto& value) {
        return value.second.phase == Impl::Phase::failed;
    });
    for (const auto& [component_id, display_name, optional] : known_components) {
        const Impl::Entry& entry = impl_->entries.at(component_id);
        const std::string verification_state = entry.phase == Impl::Phase::checking ? "checking"
                                               : entry.phase == Impl::Phase::failed ? "failed"
                                                                                    : "verified";
        json row{
            {"component_id", component_id},
            {"display_name", display_name},
            {"optional", optional},
            {"package_state", entry.phase == Impl::Phase::checking ? "checking"
                              : entry.phase == Impl::Phase::failed ? "broken"
                              : entry.component.has_value()        ? "installed"
                                                                   : "missing"},
            {"downloadable", impl_->downloadable.contains(component_id)},
            {"verification_state", verification_state},
        };
        if (entry.phase == Impl::Phase::failed || entry.phase == Impl::Phase::checking) {
            row["verification_issue"] = {
                {"code", entry.issue_code},
                {"detail", entry.issue_detail},
            };
        }
        if (entry.component) {
            const VerifiedComponent& component = *entry.component;
            row["installation_state"] = component.receipt.value("state", "");
            row["component_version"] = component.receipt.value("component_version", "");
            row["startup_installed"] =
                component.receipt.value("startup", json::object()).value("installed", false);
            if (component.manifest.contains("default_model") &&
                component.manifest.at("default_model").is_object()) {
                row["default_model"] = component.manifest.at("default_model");
            }
        }
        components.push_back(std::move(row));
    }
    return {
        {"schema", "arena_component_inventory_v1"},
        {"schema_version", 1},
        {"verification_state", any_checking ? "checking"
                               : any_failed ? "failed"
                                            : "verified"},
        {"retained_catalog_available", !impl_->downloadable.empty()},
        {"components", std::move(components)},
    };
}

VerifiedComponent ComponentVerificationRegistry::require_verified(const std::string& component_id,
                                                                  const bool allow_disabled) const {
    std::lock_guard lock(impl_->mutex);
    if (!impl_->started || !impl_->entries.contains(component_id)) {
        throw ComponentVerificationUnavailable("component_verification_checking",
                                               "Component verification has not completed.");
    }
    const Impl::Entry& entry = impl_->entries.at(component_id);
    if (entry.phase == Impl::Phase::checking) {
        throw ComponentVerificationUnavailable(entry.issue_code, entry.issue_detail);
    }
    if (entry.phase == Impl::Phase::failed) {
        throw ComponentVerificationUnavailable(entry.issue_code, entry.issue_detail);
    }
    if (!entry.component) {
        throw ComponentVerificationUnavailable("component_not_installed",
                                               "The component is not installed.");
    }
    const std::string state = entry.component->receipt.value("state", "");
    if (state != "installed_managed" && !(allow_disabled && state == "installed_disabled")) {
        throw ComponentVerificationUnavailable(
            "component_not_enabled", "The installed component is not enabled for invocation.");
    }
    return *entry.component;
}

std::vector<VerifiedComponent> ComponentVerificationRegistry::verified_components() const {
    std::lock_guard lock(impl_->mutex);
    std::vector<VerifiedComponent> result;
    result.reserve(impl_->entries.size());
    for (const auto& [component_id, entry] : impl_->entries) {
        static_cast<void>(component_id);
        if (entry.phase == Impl::Phase::verified && entry.component) {
            result.push_back(*entry.component);
        }
    }
    return result;
}

void ComponentVerificationRegistry::invalidate_all_for_testing() {
    impl_->invalidate_all();
}

std::size_t ComponentVerificationRegistry::verification_attempts_for_testing() const {
    return impl_->attempts.load();
}

} // namespace arena::application
