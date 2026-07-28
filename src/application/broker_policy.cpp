#include "arena/application/broker_policy.hpp"

#include "arena/domain/uuid.hpp"

#include <fstream>
#include <nlohmann/json.hpp>
#include <stdexcept>

namespace arena::application {

BrokerPolicy BrokerPolicy::load(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        throw std::runtime_error("broker policy cannot be opened");
    }
    nlohmann::json document;
    try {
        input >> document;
    } catch (const nlohmann::json::exception& error) {
        throw std::runtime_error(std::string("invalid broker policy: ") + error.what());
    }
    if (document.value("schema_version", 0) != 1 ||
        !document.contains("services") || !document.at("services").is_array()) {
        throw std::runtime_error("broker policy must use schema_version 1 and a services array");
    }

    BrokerPolicy policy;
    for (const auto& service : document.at("services")) {
        const std::string id = service.value("id", "");
        const std::string platform_name = service.value("platform_name", "");
        if (!domain::is_uuid(id) || platform_name.empty()) {
            throw std::runtime_error("broker service entries require UUID ids and platform names");
        }
        if (platform_name.find_first_of("\r\n\t\"';&|`$<>") != std::string::npos) {
            throw std::runtime_error("broker platform service name contains forbidden characters");
        }
        BrokerTarget target;
        target.platform_name = platform_name;
        if (!service.contains("actions") || !service.at("actions").is_array()) {
            throw std::runtime_error("broker service actions must be an array");
        }
        for (const auto& action_value : service.at("actions")) {
            const std::string action = action_value.get<std::string>();
            if (action != "start" && action != "stop" && action != "restart" &&
                action != "status") {
                throw std::runtime_error("broker policy contains an unsupported action");
            }
            target.actions.insert(action);
        }
        if (!policy.targets_.emplace(id, std::move(target)).second) {
            throw std::runtime_error("broker policy contains a duplicate service id");
        }
    }
    return policy;
}

const BrokerTarget& BrokerPolicy::authorize(
    const std::string& service_id,
    const std::string& action) const {
    const auto target = targets_.find(service_id);
    if (target == targets_.end()) {
        throw std::runtime_error("service id is not allowlisted");
    }
    if (!target->second.actions.contains(action)) {
        throw std::runtime_error("service action is not allowlisted");
    }
    return target->second;
}

std::size_t BrokerPolicy::size() const { return targets_.size(); }

}  // namespace arena::application
