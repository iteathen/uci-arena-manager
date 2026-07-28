#include "arena/application/broker_policy.hpp"

#include <CLI/CLI.hpp>
#include <nlohmann/json.hpp>

#include <fstream>
#include <iostream>
#include <stdexcept>

int main(int argc, char** argv) {
    try {
        CLI::App app{
            "Allowlisted privileged service broker; no network or general command execution"};
        std::filesystem::path policy_path;
        std::filesystem::path request_path;
        bool validate = false;
        app.add_option("--policy", policy_path, "Administrator-owned broker policy")->required();
        app.add_option("--request", request_path, "Single broker request JSON");
        app.add_flag("--validate", validate, "Validate policy without performing an action");
        CLI11_PARSE(app, argc, argv);

        const auto policy = arena::application::BrokerPolicy::load(policy_path);
        if (validate) {
            std::cout << nlohmann::json{
                {"ok", true},
                {"schema_version", 1},
                {"allowlisted_services", policy.size()},
            }.dump(2) << '\n';
            return 0;
        }
        if (request_path.empty()) {
            throw std::runtime_error("--request is required unless --validate is used");
        }
        std::ifstream input(request_path, std::ios::binary);
        nlohmann::json request;
        input >> request;
        if (request.value("schema_version", 0) != 1) {
            throw std::runtime_error("broker request schema_version must be 1");
        }
        const std::string service_id = request.value("service_id", "");
        const std::string action = request.value("action", "");
        const auto& target = policy.authorize(service_id, action);
        const int result = arena::application::perform_service_action(
            target.platform_name, action);
        std::cout << nlohmann::json{
            {"ok", result == 0},
            {"service_id", service_id},
            {"action", action},
            {"platform_result", result},
        }.dump() << '\n';
        return result == 0 ? 0 : 4;
    } catch (const CLI::ParseError& error) {
        return error.get_exit_code();
    } catch (const std::exception& error) {
        std::cerr << "arena-broker: " << error.what() << '\n';
        return 2;
    }
}
