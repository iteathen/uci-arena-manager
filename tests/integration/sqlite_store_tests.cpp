#include "arena/adapters/sqlite_store.hpp"
#include "arena/domain/uuid.hpp"

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <limits>
#include <memory>
#include <string>

namespace {

namespace fs = std::filesystem;

fs::path unique_database(const std::string& prefix) {
    return fs::temp_directory_path() /
           (prefix + arena::domain::uuid_v7() + ".db");
}

void remove_database(const fs::path& path) {
    std::error_code ignored;
    fs::remove(path, ignored);
    fs::remove(path.string() + "-wal", ignored);
    fs::remove(path.string() + "-shm", ignored);
}

void execute_raw(sqlite3* database, const char* sql) {
    char* error = nullptr;
    const int result = sqlite3_exec(database, sql, nullptr, nullptr, &error);
    const std::string message =
        error == nullptr ? std::string{} : std::string(error);
    sqlite3_free(error);
    if (result != SQLITE_OK) {
        throw std::runtime_error(message);
    }
}

} // namespace

TEST_CASE("resource command and event commit atomically") {
    const auto path = unique_database("arena-store-");
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
        REQUIRE(
            store.command_result("store-test-key") ==
            R"({"status":"completed"})");
        REQUIRE(store.events_after(0, 10).size() == 1);
        REQUIRE(store.resource_counts().at("engine") == 1);
    }
    remove_database(path);
}

TEST_CASE("SQLite storage rejects symlinked database and sidecar paths") {
    const fs::path root =
        fs::temp_directory_path() /
        ("arena-store-path-" + arena::domain::uuid_v7());
    fs::create_directories(root);

    const fs::path target = root / "target.db";
    std::ofstream(target, std::ios::binary | std::ios::trunc) << "not sqlite";
    const fs::path linked_database = root / "linked.db";
    std::error_code link_error;
    fs::create_symlink(target, linked_database, link_error);
    if (!link_error) {
        arena::adapters::SqliteStore store(linked_database);
        REQUIRE_THROWS(store.initialize());
    }

    const fs::path database = root / "sidecar.db";
    const fs::path sidecar_target = root / "sidecar-target";
    std::ofstream(sidecar_target, std::ios::binary | std::ios::trunc) << "x";
    std::error_code sidecar_error;
    fs::create_symlink(
        sidecar_target,
        database.string() + "-wal",
        sidecar_error);
    if (!sidecar_error) {
        arena::adapters::SqliteStore store(database);
        REQUIRE_THROWS(store.initialize());
    }

    fs::remove_all(root);
}

TEST_CASE("SQLite storage rejects unknown migration authority") {
    const fs::path path = unique_database("arena-store-migration-");
    sqlite3* database = nullptr;
    REQUIRE(sqlite3_open(path.string().c_str(), &database) == SQLITE_OK);
    execute_raw(
        database,
        "CREATE TABLE schema_migrations(version INTEGER PRIMARY KEY, applied_at TEXT NOT NULL);"
        "INSERT INTO schema_migrations(version, applied_at) VALUES(2, 'future');");
    REQUIRE(sqlite3_close(database) == SQLITE_OK);

    arena::adapters::SqliteStore store(path);
    REQUIRE_THROWS_WITH(
        store.initialize(),
        "SQLite database contains an unsupported schema migration");

    remove_database(path);
}

TEST_CASE("SQLite storage rejects unsigned values outside its integer domain") {
    const fs::path path = unique_database("arena-store-range-");
    {
        arena::adapters::SqliteStore store(path);
        store.initialize();
        REQUIRE_THROWS_WITH(
            store.events_after(
                (std::numeric_limits<std::uint64_t>::max)(),
                10),
            "unsigned integer exceeds SQLite signed integer range");
    }
    remove_database(path);
}
