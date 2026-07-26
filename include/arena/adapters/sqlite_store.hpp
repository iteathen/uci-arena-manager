#pragma once

#include "arena/ports/store.hpp"

#include <filesystem>
#include <mutex>
#include <sqlite3.h>

namespace arena::adapters {

class SqliteStore final : public ports::Store {
  public:
    explicit SqliteStore(std::filesystem::path database_path);
    ~SqliteStore() override;

    SqliteStore(const SqliteStore&) = delete;
    SqliteStore& operator=(const SqliteStore&) = delete;

    void initialize() override;
    [[nodiscard]] std::optional<std::string> command_result(
        const std::string& idempotency_key) const override;
    void create_resource(
        const ports::ResourceRecord& resource,
        const ports::CommandRecord& command,
        ports::EventRecord event) override;
    void update_resource(
        const ports::ResourceRecord& resource,
        std::uint64_t expected_revision,
        const ports::CommandRecord& command,
        ports::EventRecord event) override;
    void delete_resource(
        const std::string& kind,
        const std::string& id,
        std::uint64_t expected_revision,
        const ports::CommandRecord& command,
        ports::EventRecord event) override;
    [[nodiscard]] std::optional<ports::ResourceRecord> get_resource(
        const std::string& kind,
        const std::string& id) const override;
    [[nodiscard]] std::vector<ports::ResourceRecord> list_resources(
        const std::string& kind,
        std::uint32_t limit,
        std::uint32_t offset) const override;
    [[nodiscard]] std::map<std::string, std::uint64_t> resource_counts() const override;
    [[nodiscard]] std::vector<ports::EventRecord> events_after(
        std::uint64_t cursor,
        std::uint32_t limit) const override;

  private:
    void execute(const char* sql) const;
    void begin() const;
    void commit() const;
    void rollback() const noexcept;
    void insert_command(const ports::CommandRecord& command) const;
    void insert_event(const ports::EventRecord& event) const;

    std::filesystem::path database_path_;
    mutable std::mutex mutex_;
    mutable sqlite3* database_{nullptr};
};

}  // namespace arena::adapters
