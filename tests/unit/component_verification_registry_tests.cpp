#include "arena/application/component_verification_registry.hpp"
#include "arena/domain/uuid.hpp"
#include "arena/ports/component_change_monitor.hpp"

#include <sodium.h>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <future>
#include <nlohmann/json.hpp>
#include <thread>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

namespace {

namespace fs = std::filesystem;
using arena::application::ComponentVerificationRegistry;
using arena::application::ComponentVerificationUnavailable;
using arena::application::VerifiedComponent;
using nlohmann::json;

std::string sha256_text(const std::string& value) {
    std::array<unsigned char, crypto_hash_sha256_BYTES> digest{};
    crypto_hash_sha256(digest.data(), reinterpret_cast<const unsigned char*>(value.data()),
                       static_cast<unsigned long long>(value.size()));
    std::array<char, crypto_hash_sha256_BYTES * 2U + 1U> encoded{};
    sodium_bin2hex(encoded.data(), encoded.size(), digest.data(), digest.size());
    return encoded.data();
}

void write_json(const fs::path& path, const json& value) {
    fs::create_directories(path.parent_path());
    std::ofstream(path, std::ios::binary | std::ios::trunc) << value.dump(2) << '\n';
}

struct Fixture final {
    fs::path root;
    fs::path data;
    fs::path active;
    fs::path payload;
    json receipt;
    json manifest;
};

Fixture make_fixture(const std::string& component_id = "uci_arena.engine") {
    Fixture fixture;
    fixture.root =
        fs::temp_directory_path() / ("arena-component-registry-" + arena::domain::uuid_v7());
    fixture.data = fixture.root / "data";
    fixture.active = fixture.root / "install" / component_id / "1.0.0";
    fixture.payload = fixture.active / "bin" / "engine.exe";
    fs::create_directories(fixture.payload.parent_path());
    const std::string bytes = "verified fixture payload";
    std::ofstream(fixture.payload, std::ios::binary) << bytes;
    fixture.manifest = {
        {"schema", "arena_install_component_v1"},
        {"schema_version", 1},
        {"component_id", component_id},
        {"component_version", "1.0.0"},
        {"entrypoints", {{"uci", "bin/engine.exe"}}},
        {"files", json::array({
                      {
                          {"path", "bin/engine.exe"},
                          {"size_bytes", bytes.size()},
                          {"sha256", sha256_text(bytes)},
                      },
                  })},
    };
    fixture.receipt = {
        {"schema", "arena_install_receipt_v1"},
        {"schema_version", 1},
        {"component_id", component_id},
        {"component_version", "1.0.0"},
        {"state", "installed_managed"},
        {"active_root", fixture.active.string()},
        {"artifact_sha256", std::string(64, 'a')},
    };
    write_json(fixture.active / "arena-component.json", fixture.manifest);
    write_json(fixture.data / "receipts" / (component_id + ".json"), fixture.receipt);
    return fixture;
}

bool wait_until(const std::function<bool()>& predicate,
                const std::chrono::milliseconds timeout = std::chrono::seconds(5)) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (predicate()) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return predicate();
}

} // namespace

TEST_CASE("component verification starts asynchronously and inventory reads never rehash") {
    Fixture fixture = make_fixture();
    std::promise<void> target_started;
    std::promise<void> release_promise;
    std::shared_future<void> release = release_promise.get_future().share();
    std::atomic_bool announced{false};
    ComponentVerificationRegistry registry(
        [&](const fs::path&, const std::string& component_id, bool) {
            if (component_id != "uci_arena.engine") {
                throw ComponentVerificationUnavailable("component_not_installed",
                                                       "The component is not installed.");
            }
            if (!announced.exchange(true)) {
                target_started.set_value();
            }
            release.wait();
            return VerifiedComponent{fixture.receipt, fixture.manifest, fixture.active};
        });

    registry.begin(fixture.data);
    REQUIRE(target_started.get_future().wait_for(std::chrono::seconds(5)) ==
            std::future_status::ready);
    REQUIRE(registry.inventory().at("verification_state") == "checking");
    const std::size_t attempts = registry.verification_attempts_for_testing();
    for (int index = 0; index < 50; ++index) {
        REQUIRE(registry.inventory().at("verification_state") == "checking");
    }
    REQUIRE(registry.verification_attempts_for_testing() == attempts);

    release_promise.set_value();
    REQUIRE(registry.wait_for_initial_verification());
    REQUIRE(registry.inventory().at("verification_state") == "verified");
    const std::size_t completed_attempts = registry.verification_attempts_for_testing();
    for (int index = 0; index < 50; ++index) {
        static_cast<void>(registry.inventory());
    }
    REQUIRE(registry.verification_attempts_for_testing() == completed_attempts);
    registry.stop();
    fs::remove_all(fixture.root);
}

TEST_CASE("component mutation invalidates invocation until exact revalidation succeeds") {
    Fixture fixture = make_fixture();
    ComponentVerificationRegistry registry;
    registry.begin(fixture.data);
    REQUIRE(registry.wait_for_initial_verification());
    REQUIRE(wait_until([&] {
        try {
            static_cast<void>(registry.require_verified("uci_arena.engine"));
            return true;
        } catch (const ComponentVerificationUnavailable&) {
            return false;
        }
    }));

    std::ofstream(fixture.payload, std::ios::binary | std::ios::trunc)
        << "tampered fixture payload";
    REQUIRE(
        wait_until([&] { return registry.inventory().at("verification_state") != "verified"; }));
    REQUIRE_THROWS_AS(registry.require_verified("uci_arena.engine"),
                      ComponentVerificationUnavailable);
    const std::string projection = registry.inventory().dump();
    REQUIRE(projection.find(fixture.root.string()) == std::string::npos);

    const std::string original = "verified fixture payload";
    std::ofstream(fixture.payload, std::ios::binary | std::ios::trunc) << original;
    REQUIRE(wait_until([&] {
        try {
            static_cast<void>(registry.require_verified("uci_arena.engine"));
            return true;
        } catch (const ComponentVerificationUnavailable&) {
            return false;
        }
    }));
    registry.stop();
    fs::remove_all(fixture.root);
}

TEST_CASE("watcher overflow invalidates all component invocation authority") {
    Fixture fixture = make_fixture();
    std::promise<void> recheck_started;
    std::promise<void> release_recheck;
    std::shared_future<void> release = release_recheck.get_future().share();
    std::atomic_size_t engine_attempts{0};
    ComponentVerificationRegistry registry(
        [&](const fs::path&, const std::string& component_id, bool) {
            if (component_id != "uci_arena.engine") {
                throw ComponentVerificationUnavailable("component_not_installed",
                                                       "The component is not installed.");
            }
            if (++engine_attempts > 1U) {
                recheck_started.set_value();
                release.wait();
            }
            return VerifiedComponent{fixture.receipt, fixture.manifest, fixture.active};
        });
    registry.begin(fixture.data);
    REQUIRE(registry.wait_for_initial_verification());
    REQUIRE_NOTHROW(registry.require_verified("uci_arena.engine"));

    registry.invalidate_all_for_testing();
    REQUIRE(recheck_started.get_future().wait_for(std::chrono::seconds(5)) ==
            std::future_status::ready);
    REQUIRE(registry.inventory().at("verification_state") == "checking");
    try {
        static_cast<void>(registry.require_verified("uci_arena.engine"));
        FAIL("overflow retained component invocation authority");
    } catch (const ComponentVerificationUnavailable& failure) {
        REQUIRE(failure.code() == "component_watcher_overflow");
    }
    release_recheck.set_value();
    REQUIRE(wait_until([&] {
        try {
            static_cast<void>(registry.require_verified("uci_arena.engine"));
            return true;
        } catch (const ComponentVerificationUnavailable&) {
            return false;
        }
    }));
    registry.stop();
    fs::remove_all(fixture.root);
}

TEST_CASE("aggregate verification includes substitutable receipt components") {
    Fixture fixture = make_fixture("vendor.engine");
    std::promise<void> recheck_started;
    std::promise<void> release_recheck;
    std::shared_future<void> release = release_recheck.get_future().share();
    std::atomic_size_t vendor_attempts{0};
    std::atomic_bool announced{false};
    ComponentVerificationRegistry registry(
        [&](const fs::path&, const std::string& component_id, bool) {
            if (component_id != "vendor.engine") {
                throw ComponentVerificationUnavailable("component_not_installed",
                                                       "The component is not installed.");
            }
            if (++vendor_attempts > 1U) {
                if (!announced.exchange(true)) {
                    recheck_started.set_value();
                }
                release.wait();
            }
            return VerifiedComponent{fixture.receipt, fixture.manifest, fixture.active};
        });
    registry.begin(fixture.data);
    REQUIRE(registry.wait_for_initial_verification());
    REQUIRE_NOTHROW(registry.require_verified("vendor.engine"));
    REQUIRE(registry.inventory().at("verification_state") == "verified");

    std::ofstream(fixture.payload, std::ios::binary | std::ios::trunc) << "changed vendor payload";
    REQUIRE(recheck_started.get_future().wait_for(std::chrono::seconds(5)) ==
            std::future_status::ready);
    REQUIRE(registry.inventory().at("verification_state") == "checking");
    REQUIRE_THROWS_AS(registry.require_verified("vendor.engine"), ComponentVerificationUnavailable);

    release_recheck.set_value();
    REQUIRE(wait_until([&] {
        try {
            static_cast<void>(registry.require_verified("vendor.engine"));
            return true;
        } catch (const ComponentVerificationUnavailable&) {
            return false;
        }
    }));
    registry.stop();
    fs::remove_all(fixture.root);
}

#ifdef _WIN32
TEST_CASE("component monitoring captures changes before its worker first runs") {
    Fixture fixture = make_fixture();
    struct SchedulingScope {
        DWORD_PTR affinity{};
        int priority{GetThreadPriority(GetCurrentThread())};
        ~SchedulingScope() {
            SetThreadPriority(GetCurrentThread(), priority);
            if (affinity != 0) SetProcessAffinityMask(GetCurrentProcess(), affinity);
        }
    } scheduling;
    DWORD_PTR system_affinity = 0;
    REQUIRE(GetProcessAffinityMask(GetCurrentProcess(), &scheduling.affinity,
                                   &system_affinity));
    const DWORD_PTR one_cpu = scheduling.affinity & (~scheduling.affinity + 1);
    REQUIRE(SetProcessAffinityMask(GetCurrentProcess(), one_cpu));
    REQUIRE(SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST));

    std::promise<void> changed;
    auto delivered = changed.get_future();
    std::atomic_bool announced{false};
    auto monitor = arena::ports::watch_component_tree(
        fixture.active, [&](const arena::ports::ComponentChange& change) {
            if (change.kind == arena::ports::ComponentChangeKind::changed &&
                change.relative_path == fs::path("bin") / "engine.exe" &&
                !announced.exchange(true)) {
                changed.set_value();
            }
        });
    REQUIRE(monitor != nullptr);
    std::ofstream(fixture.payload, std::ios::binary | std::ios::trunc)
        << "immediate changed payload";
    const bool observed = delivered.wait_for(std::chrono::seconds(2)) ==
                          std::future_status::ready;
    monitor.reset();
    fs::remove_all(fixture.root);
    REQUIRE(observed);
}
#endif
