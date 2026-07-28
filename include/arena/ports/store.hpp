#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace arena::ports {

struct ResourceRecord {
    std::string id;
    std::string kind;
    std::string host_id{"local"};
    std::uint64_t revision{1};
    std::string created_at;
    std::string updated_at;
    std::string labels_json{"{}"};
    std::string data_json{"{}"};
};

struct CommandRecord {
    std::string id;
    std::string request_id;
    std::string idempotency_key;
    std::string action;
    std::string status;
    std::string result_json;
    std::string created_at;
};

struct EventRecord {
    std::uint64_t cursor{};
    std::string event_id;
    std::string timestamp;
    std::string stream;
    std::string kind;
    std::optional<std::string> resource_kind;
    std::optional<std::string> resource_id;
    std::optional<std::uint64_t> resource_revision;
    std::optional<std::string> correlation_id;
    std::string payload_json{"{}"};
};

class Store {
  public:
    virtual ~Store() = default;

    virtual void initialize() = 0;
    [[nodiscard]] virtual std::optional<std::string> command_result(
        const std::string& idempotency_key) const = 0;
    virtual void create_resource(
        const ResourceRecord& resource,
        const CommandRecord& command,
        EventRecord event) = 0;
    virtual void update_resource(
        const ResourceRecord& resource,
        std::uint64_t expected_revision,
        const CommandRecord& command,
        EventRecord event) = 0;
    virtual void delete_resource(
        const std::string& kind,
        const std::string& id,
        std::uint64_t expected_revision,
        const CommandRecord& command,
        EventRecord event) = 0;
    [[nodiscard]] virtual std::optional<ResourceRecord> get_resource(
        const std::string& kind,
        const std::string& id) const = 0;
    [[nodiscard]] virtual std::vector<ResourceRecord> list_resources(
        const std::string& kind,
        std::uint32_t limit,
        std::uint32_t offset) const = 0;
    [[nodiscard]] virtual std::map<std::string, std::uint64_t> resource_counts() const = 0;
    [[nodiscard]] virtual std::vector<EventRecord> events_after(
        std::uint64_t cursor,
        std::uint32_t limit) const = 0;
};

}  // namespace arena::ports
