#include "arena/adapters/sqlite_store.hpp"
#include "arena/domain/uuid.hpp"

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <memory>

TEST_CASE("resource command and event commit atomically") {
    const auto path =
        std::filesystem::temp_directory_path() /
        ("arena-store-" + arena::domain::uuid_v7() + ".db");
    {
        arena::adapters::SqliteStore store(path);
        store.initialize();
        const std::string now = arena::domain::utc_now();
        const std::string resource_id = arena::domain::uuid_v7();
        const std::string command_id = arena::domain::uuid_v7();
        store.create_resource(
            {
                resource_id,
                "engine",
                "local",
                1,
                now,
                now,
                "{}",
                R"({"display_name":"Mock UCI"})",
            },
            {
                command_id,
                arena::domain::uuid_v7(),
                "store-test-key",
                "engine.register",
                "completed",
                R"({"status":"completed"})",
                now,
            },
            {
                0,
                arena::domain::uuid_v7(),
                now,
                "state",
                "engine.created",
                "engine",
                resource_id,
                1,
                command_id,
                "{}",
            });
        const auto resource = store.get_resource("engine", resource_id);
        REQUIRE(resource);
        REQUIRE(resource->revision == 1);
        REQUIRE(store.command_result("store-test-key") == R"({"status":"completed"})");
        REQUIRE(store.events_after(0, 10).size() == 1);
        REQUIRE(store.resource_counts().at("engine") == 1);
    }
    std::filesystem::remove(path);
    std::filesystem::remove(path.string() + "-wal");
    std::filesystem::remove(path.string() + "-shm");
}
