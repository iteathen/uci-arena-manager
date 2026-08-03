#include "arena/adapters/sqlite_store.hpp"

#include <cstdint>
#include <filesystem>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>

namespace arena::adapters {
namespace {

class Statement final {
  public:
    Statement(sqlite3* database, const char* sql) : database_(database) {
        const int result =
            sqlite3_prepare_v2(database, sql, -1, &statement_, nullptr);
        if (result != SQLITE_OK) {
            throw std::runtime_error(sqlite3_errmsg(database));
        }
    }

    ~Statement() {
        if (statement_ != nullptr) {
            sqlite3_finalize(statement_);
        }
    }

    Statement(const Statement&) = delete;
    Statement& operator=(const Statement&) = delete;

    sqlite3_stmt* get() const { return statement_; }

    void bind(const int index, const std::string& value) const {
        if (sqlite3_bind_text(
                statement_, index, value.c_str(), -1, SQLITE_TRANSIENT) !=
            SQLITE_OK) {
            throw std::runtime_error(sqlite3_errmsg(database_));
        }
    }

    void bind(const int index, const std::uint64_t value) const {
        if (value >
            static_cast<std::uint64_t>(
                (std::numeric_limits<sqlite3_int64>::max)())) {
            throw std::runtime_error(
                "unsigned integer exceeds SQLite signed integer range");
        }
        if (sqlite3_bind_int64(
                statement_,
                index,
                static_cast<sqlite3_int64>(value)) != SQLITE_OK) {
            throw std::runtime_error(sqlite3_errmsg(database_));
        }
    }

    void bind_optional(
        const int index,
        const std::optional<std::string>& value) const {
        if (value) {
            bind(index, *value);
        } else if (sqlite3_bind_null(statement_, index) != SQLITE_OK) {
            throw std::runtime_error(sqlite3_errmsg(database_));
        }
    }

    void bind_optional(
        const int index,
        const std::optional<std::uint64_t>& value) const {
        if (value) {
            bind(index, *value);
        } else if (sqlite3_bind_null(statement_, index) != SQLITE_OK) {
            throw std::runtime_error(sqlite3_errmsg(database_));
        }
    }

    int step() const { return sqlite3_step(statement_); }

    void expect_done() const {
        const int result = step();
        if (result != SQLITE_DONE) {
            throw std::runtime_error(sqlite3_errmsg(database_));
        }
    }

  private:
    sqlite3* database_{};
    sqlite3_stmt* statement_{};
};

std::string column_text(sqlite3_stmt* statement, const int column) {
    const auto* value = sqlite3_column_text(statement, column);
    return value == nullptr
        ? std::string{}
        : reinterpret_cast<const char*>(value);
}

std::uint64_t required_uint64(sqlite3_stmt* statement, const int column) {
    const sqlite3_int64 value = sqlite3_column_int64(statement, column);
    if (value < 0) {
        throw std::runtime_error(
            "SQLite row contains a negative unsigned integer");
    }
    return static_cast<std::uint64_t>(value);
}

std::optional<std::string> optional_text(
    sqlite3_stmt* statement,
    const int column) {
    if (sqlite3_column_type(statement, column) == SQLITE_NULL) {
        return std::nullopt;
    }
    return column_text(statement, column);
}

std::optional<std::uint64_t> optional_uint64(
    sqlite3_stmt* statement,
    const int column) {
    if (sqlite3_column_type(statement, column) == SQLITE_NULL) {
        return std::nullopt;
    }
    return required_uint64(statement, column);
}

ports::ResourceRecord resource_from_row(sqlite3_stmt* row) {
    return ports::ResourceRecord{
        column_text(row, 0),
        column_text(row, 1),
        column_text(row, 2),
        required_uint64(row, 3),
        column_text(row, 4),
        column_text(row, 5),
        column_text(row, 6),
        column_text(row, 7),
    };
}

ports::EventRecord event_from_row(sqlite3_stmt* row) {
    return ports::EventRecord{
        required_uint64(row, 0),
        column_text(row, 1),
        column_text(row, 2),
        column_text(row, 3),
        column_text(row, 4),
        optional_text(row, 5),
        optional_text(row, 6),
        optional_uint64(row, 7),
        optional_text(row, 8),
        column_text(row, 9),
    };
}

void require_safe_file_state(
    const std::filesystem::path& path,
    const bool allow_missing,
    const char* label) {
    std::error_code error;
    const auto status = std::filesystem::symlink_status(path, error);
    if (!error && status.type() == std::filesystem::file_type::not_found &&
        allow_missing) {
        return;
    }
    if (error) {
        if (allow_missing && error == std::errc::no_such_file_or_directory) {
            return;
        }
        throw std::runtime_error(
            std::string(label) + " cannot be inspected");
    }
    if (!std::filesystem::is_regular_file(status) ||
        std::filesystem::is_symlink(status)) {
        throw std::runtime_error(
            std::string(label) +
            " must be a regular non-symlink file");
    }
}

void validate_database_paths(const std::filesystem::path& database_path) {
    if (database_path.empty()) {
        throw std::runtime_error("SQLite database path is required");
    }
    if (database_path.has_parent_path()) {
        std::filesystem::create_directories(database_path.parent_path());
        std::error_code error;
        const auto parent_status = std::filesystem::symlink_status(
            database_path.parent_path(), error);
        if (error || !std::filesystem::is_directory(parent_status) ||
            std::filesystem::is_symlink(parent_status)) {
            throw std::runtime_error(
                "SQLite database directory must be a non-symlink directory");
        }
    }
    require_safe_file_state(
        database_path, true, "SQLite database");
    require_safe_file_state(
        database_path.string() + "-wal", true, "SQLite WAL sidecar");
    require_safe_file_state(
        database_path.string() + "-shm", true, "SQLite SHM sidecar");
}

std::string pragma_text(sqlite3* database, const char* sql) {
    Statement statement(database, sql);
    if (statement.step() != SQLITE_ROW) {
        throw std::runtime_error(
            std::string("SQLite pragma returned no row: ") + sql);
    }
    return column_text(statement.get(), 0);
}

std::int64_t pragma_integer(sqlite3* database, const char* sql) {
    Statement statement(database, sql);
    if (statement.step() != SQLITE_ROW) {
        throw std::runtime_error(
            std::string("SQLite pragma returned no row: ") + sql);
    }
    return sqlite3_column_int64(statement.get(), 0);
}

void validate_migration_state(sqlite3* database) {
    Statement statement(
        database,
        "SELECT version FROM schema_migrations ORDER BY version ASC;");
    while (true) {
        const int result = statement.step();
        if (result == SQLITE_DONE) {
            return;
        }
        if (result != SQLITE_ROW) {
            throw std::runtime_error(sqlite3_errmsg(database));
        }
        if (sqlite3_column_int64(statement.get(), 0) != 1) {
            throw std::runtime_error(
                "SQLite database contains an unsupported schema migration");
        }
    }
}

}  // namespace

SqliteStore::SqliteStore(std::filesystem::path database_path)
    : database_path_(std::move(database_path)) {}

SqliteStore::~SqliteStore() {
    std::scoped_lock lock(mutex_);
    if (database_ != nullptr) {
        sqlite3_close_v2(database_);
        database_ = nullptr;
    }
}

void SqliteStore::initialize() {
    std::scoped_lock lock(mutex_);
    if (database_ != nullptr) {
        return;
    }
    validate_database_paths(database_path_);

    int flags =
        SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX;
#ifdef SQLITE_OPEN_NOFOLLOW
    flags |= SQLITE_OPEN_NOFOLLOW;
#endif
    if (sqlite3_open_v2(
            database_path_.string().c_str(),
            &database_,
            flags,
            nullptr) != SQLITE_OK) {
        const std::string error = database_ == nullptr
            ? "sqlite open failed"
            : sqlite3_errmsg(database_);
        if (database_ != nullptr) {
            sqlite3_close_v2(database_);
            database_ = nullptr;
        }
        throw std::runtime_error(error);
    }

    try {
        sqlite3_extended_result_codes(database_, 1);
        execute("PRAGMA journal_mode=WAL;");
        execute("PRAGMA synchronous=FULL;");
        execute("PRAGMA foreign_keys=ON;");
        execute("PRAGMA busy_timeout=5000;");
        if (pragma_text(database_, "PRAGMA journal_mode;") != "wal") {
            throw std::runtime_error(
                "SQLite WAL journal mode could not be enabled");
        }
        if (pragma_integer(database_, "PRAGMA foreign_keys;") != 1) {
            throw std::runtime_error(
                "SQLite foreign-key enforcement could not be enabled");
        }
        if (pragma_integer(database_, "PRAGMA synchronous;") != 2) {
            throw std::runtime_error(
                "SQLite FULL synchronous mode could not be enabled");
        }

        begin();
        try {
            execute(
                "CREATE TABLE IF NOT EXISTS schema_migrations("
                " version INTEGER PRIMARY KEY,"
                " applied_at TEXT NOT NULL"
                ");");
            validate_migration_state(database_);
            execute(
                "CREATE TABLE IF NOT EXISTS resources("
                " id TEXT NOT NULL,"
                " kind TEXT NOT NULL,"
                " host_id TEXT NOT NULL CHECK(host_id='local'),"
                " revision INTEGER NOT NULL CHECK(revision > 0),"
                " created_at TEXT NOT NULL,"
                " updated_at TEXT NOT NULL,"
                " labels_json TEXT NOT NULL,"
                " data_json TEXT NOT NULL,"
                " PRIMARY KEY(kind, id)"
                ");");
            execute(
                "CREATE INDEX IF NOT EXISTS resources_kind_updated"
                " ON resources(kind, updated_at DESC);");
            execute(
                "CREATE TABLE IF NOT EXISTS commands("
                " id TEXT PRIMARY KEY,"
                " request_id TEXT NOT NULL UNIQUE,"
                " idempotency_key TEXT NOT NULL UNIQUE,"
                " action TEXT NOT NULL,"
                " status TEXT NOT NULL,"
                " result_json TEXT NOT NULL,"
                " created_at TEXT NOT NULL"
                ");");
            execute(
                "CREATE TABLE IF NOT EXISTS events("
                " cursor INTEGER PRIMARY KEY AUTOINCREMENT,"
                " event_id TEXT NOT NULL UNIQUE,"
                " timestamp TEXT NOT NULL,"
                " stream TEXT NOT NULL,"
                " kind TEXT NOT NULL,"
                " resource_kind TEXT,"
                " resource_id TEXT,"
                " resource_revision INTEGER,"
                " correlation_id TEXT,"
                " payload_json TEXT NOT NULL"
                ");");
            execute(
                "CREATE INDEX IF NOT EXISTS events_resource"
                " ON events(resource_kind, resource_id, cursor);");
            execute(
                "INSERT OR IGNORE INTO schema_migrations(version, applied_at)"
                " VALUES(1, strftime('%Y-%m-%dT%H:%M:%fZ','now'));");
            commit();
        } catch (...) {
            rollback();
            throw;
        }
    } catch (...) {
        sqlite3_close_v2(database_);
        database_ = nullptr;
        throw;
    }
}

void SqliteStore::execute(const char* sql) const {
    char* error = nullptr;
    const int result =
        sqlite3_exec(database_, sql, nullptr, nullptr, &error);
    if (result != SQLITE_OK) {
        const std::string message = error == nullptr
            ? sqlite3_errmsg(database_)
            : error;
        sqlite3_free(error);
        throw std::runtime_error(message);
    }
}

void SqliteStore::begin() const { execute("BEGIN IMMEDIATE;"); }
void SqliteStore::commit() const { execute("COMMIT;"); }
void SqliteStore::rollback() const noexcept {
    sqlite3_exec(database_, "ROLLBACK;", nullptr, nullptr, nullptr);
}

std::optional<std::string> SqliteStore::command_result(
    const std::string& idempotency_key) const {
    std::scoped_lock lock(mutex_);
    Statement statement(
        database_,
        "SELECT result_json FROM commands WHERE idempotency_key=?1;");
    statement.bind(1, idempotency_key);
    const int result = statement.step();
    if (result == SQLITE_ROW) {
        return column_text(statement.get(), 0);
    }
    if (result != SQLITE_DONE) {
        throw std::runtime_error(sqlite3_errmsg(database_));
    }
    return std::nullopt;
}

void SqliteStore::insert_command(
    const ports::CommandRecord& command) const {
    Statement statement(
        database_,
        "INSERT INTO commands(id, request_id, idempotency_key, action, status, result_json, created_at)"
        " VALUES(?1, ?2, ?3, ?4, ?5, ?6, ?7);");
    statement.bind(1, command.id);
    statement.bind(2, command.request_id);
    statement.bind(3, command.idempotency_key);
    statement.bind(4, command.action);
    statement.bind(5, command.status);
    statement.bind(6, command.result_json);
    statement.bind(7, command.created_at);
    statement.expect_done();
}

void SqliteStore::insert_event(
    const ports::EventRecord& event) const {
    Statement statement(
        database_,
        "INSERT INTO events(event_id, timestamp, stream, kind, resource_kind, resource_id,"
        " resource_revision, correlation_id, payload_json)"
        " VALUES(?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9);");
    statement.bind(1, event.event_id);
    statement.bind(2, event.timestamp);
    statement.bind(3, event.stream);
    statement.bind(4, event.kind);
    statement.bind_optional(5, event.resource_kind);
    statement.bind_optional(6, event.resource_id);
    statement.bind_optional(7, event.resource_revision);
    statement.bind_optional(8, event.correlation_id);
    statement.bind(9, event.payload_json);
    statement.expect_done();
}

void SqliteStore::create_resource(
    const ports::ResourceRecord& resource,
    const ports::CommandRecord& command,
    ports::EventRecord event) {
    std::scoped_lock lock(mutex_);
    begin();
    try {
        Statement statement(
            database_,
            "INSERT INTO resources(id, kind, host_id, revision, created_at, updated_at,"
            " labels_json, data_json) VALUES(?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8);");
        statement.bind(1, resource.id);
        statement.bind(2, resource.kind);
        statement.bind(3, resource.host_id);
        statement.bind(4, resource.revision);
        statement.bind(5, resource.created_at);
        statement.bind(6, resource.updated_at);
        statement.bind(7, resource.labels_json);
        statement.bind(8, resource.data_json);
        statement.expect_done();
        insert_command(command);
        insert_event(event);
        commit();
    } catch (...) {
        rollback();
        throw;
    }
}

void SqliteStore::update_resource(
    const ports::ResourceRecord& resource,
    const std::uint64_t expected_revision,
    const ports::CommandRecord& command,
    ports::EventRecord event) {
    std::scoped_lock lock(mutex_);
    begin();
    try {
        Statement statement(
            database_,
            "UPDATE resources SET revision=?1, updated_at=?2, labels_json=?3, data_json=?4"
            " WHERE kind=?5 AND id=?6 AND revision=?7;");
        statement.bind(1, resource.revision);
        statement.bind(2, resource.updated_at);
        statement.bind(3, resource.labels_json);
        statement.bind(4, resource.data_json);
        statement.bind(5, resource.kind);
        statement.bind(6, resource.id);
        statement.bind(7, expected_revision);
        statement.expect_done();
        if (sqlite3_changes(database_) != 1) {
            throw std::runtime_error("resource revision conflict");
        }
        insert_command(command);
        insert_event(event);
        commit();
    } catch (...) {
        rollback();
        throw;
    }
}

void SqliteStore::delete_resource(
    const std::string& kind,
    const std::string& id,
    const std::uint64_t expected_revision,
    const ports::CommandRecord& command,
    ports::EventRecord event) {
    std::scoped_lock lock(mutex_);
    begin();
    try {
        Statement statement(
            database_,
            "DELETE FROM resources WHERE kind=?1 AND id=?2 AND revision=?3;");
        statement.bind(1, kind);
        statement.bind(2, id);
        statement.bind(3, expected_revision);
        statement.expect_done();
        if (sqlite3_changes(database_) != 1) {
            throw std::runtime_error("resource revision conflict");
        }
        insert_command(command);
        insert_event(event);
        commit();
    } catch (...) {
        rollback();
        throw;
    }
}

std::optional<ports::ResourceRecord> SqliteStore::get_resource(
    const std::string& kind,
    const std::string& id) const {
    std::scoped_lock lock(mutex_);
    Statement statement(
        database_,
        "SELECT id, kind, host_id, revision, created_at, updated_at, labels_json, data_json"
        " FROM resources WHERE kind=?1 AND id=?2;");
    statement.bind(1, kind);
    statement.bind(2, id);
    const int result = statement.step();
    if (result == SQLITE_ROW) {
        return resource_from_row(statement.get());
    }
    if (result != SQLITE_DONE) {
        throw std::runtime_error(sqlite3_errmsg(database_));
    }
    return std::nullopt;
}

std::vector<ports::ResourceRecord> SqliteStore::list_resources(
    const std::string& kind,
    const std::uint32_t limit,
    const std::uint32_t offset) const {
    std::scoped_lock lock(mutex_);
    Statement statement(
        database_,
        "SELECT id, kind, host_id, revision, created_at, updated_at, labels_json, data_json"
        " FROM resources WHERE kind=?1 ORDER BY updated_at DESC LIMIT ?2 OFFSET ?3;");
    statement.bind(1, kind);
    statement.bind(2, limit);
    statement.bind(3, offset);
    std::vector<ports::ResourceRecord> resources;
    while (true) {
        const int result = statement.step();
        if (result == SQLITE_ROW) {
            resources.push_back(resource_from_row(statement.get()));
            continue;
        }
        if (result != SQLITE_DONE) {
            throw std::runtime_error(sqlite3_errmsg(database_));
        }
        break;
    }
    return resources;
}

std::map<std::string, std::uint64_t> SqliteStore::resource_counts() const {
    std::scoped_lock lock(mutex_);
    Statement statement(
        database_,
        "SELECT kind, COUNT(*) FROM resources GROUP BY kind;");
    std::map<std::string, std::uint64_t> counts;
    while (true) {
        const int result = statement.step();
        if (result == SQLITE_ROW) {
            counts.emplace(
                column_text(statement.get(), 0),
                required_uint64(statement.get(), 1));
            continue;
        }
        if (result != SQLITE_DONE) {
            throw std::runtime_error(sqlite3_errmsg(database_));
        }
        break;
    }
    return counts;
}

std::vector<ports::EventRecord> SqliteStore::events_after(
    const std::uint64_t cursor,
    const std::uint32_t limit) const {
    std::scoped_lock lock(mutex_);
    Statement statement(
        database_,
        "SELECT cursor, event_id, timestamp, stream, kind, resource_kind, resource_id,"
        " resource_revision, correlation_id, payload_json"
        " FROM events WHERE cursor>?1 ORDER BY cursor ASC LIMIT ?2;");
    statement.bind(1, cursor);
    statement.bind(2, limit);
    std::vector<ports::EventRecord> events;
    while (true) {
        const int result = statement.step();
        if (result == SQLITE_ROW) {
            events.push_back(event_from_row(statement.get()));
            continue;
        }
        if (result != SQLITE_DONE) {
            throw std::runtime_error(sqlite3_errmsg(database_));
        }
        break;
    }
    return events;
}

}  // namespace arena::adapters
