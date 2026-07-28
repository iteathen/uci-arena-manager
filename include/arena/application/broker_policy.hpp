#pragma once

#include <filesystem>
#include <map>
#include <set>
#include <string>

namespace arena::application {

struct BrokerTarget {
    std::string platform_name;
    std::set<std::string, std::less<>> actions;
};

class BrokerPolicy final {
  public:
    static BrokerPolicy load(const std::filesystem::path& path);

    [[nodiscard]] const BrokerTarget& authorize(
        const std::string& service_id,
        const std::string& action) const;
    [[nodiscard]] std::size_t size() const;

  private:
    std::map<std::string, BrokerTarget, std::less<>> targets_;
};

[[nodiscard]] int perform_service_action(
    const std::string& platform_name,
    const std::string& action);

}  // namespace arena::application
