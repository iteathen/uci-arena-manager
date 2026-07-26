#include "arena/adapters/uci_process.hpp"
#include "arena/application/broker_policy.hpp"
#include "arena/domain/uuid.hpp"

#include <CLI/CLI.hpp>
#include <curl/curl.h>
#include <nlohmann/json.hpp>

#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

#ifdef _WIN32
#define NOMINMAX
#include <shellapi.h>
#include <windows.h>
#else
#include <sys/types.h>
#include <unistd.h>
#endif

namespace {

using nlohmann::json;

struct HttpResult {
    long status{};
    std::string body;
};

std::size_t receive_body(char* data, std::size_t size, std::size_t count, void* target) {
    static_cast<std::string*>(target)->append(data, size * count);
    return size * count;
}

class HttpClient final {
  public:
    HttpClient(std::string endpoint, const std::filesystem::path& key_path)
        : endpoint_(std::move(endpoint)) {
        while (!endpoint_.empty() && endpoint_.back() == '/') {
            endpoint_.pop_back();
        }
        std::ifstream input(key_path, std::ios::binary);
        std::getline(input, key_);
        if (key_.empty()) {
            throw std::runtime_error(
                "client key is unavailable; start arena-service or set --data-dir");
        }
    }

    [[nodiscard]] json get(const std::string& path) const {
        return request("GET", path, std::nullopt);
    }

    [[nodiscard]] json post(const std::string& path, const json& body) const {
        return request("POST", path, body.dump());
    }

  private:
    [[nodiscard]] json request(
        const std::string& method,
        const std::string& path,
        const std::optional<std::string>& body) const {
        CURL* handle = curl_easy_init();
        if (handle == nullptr) {
            throw std::runtime_error("cannot initialize HTTP client");
        }
        HttpResult result;
        curl_slist* headers = nullptr;
        const std::string authorization = "Authorization: Bearer " + key_;
        headers = curl_slist_append(headers, authorization.c_str());
        headers = curl_slist_append(headers, "Accept: application/json");
        if (body) {
            headers = curl_slist_append(headers, "Content-Type: application/json");
        }
        const std::string url = endpoint_ + path;
        curl_easy_setopt(handle, CURLOPT_URL, url.c_str());
        curl_easy_setopt(handle, CURLOPT_HTTPHEADER, headers);
        curl_easy_setopt(handle, CURLOPT_WRITEFUNCTION, receive_body);
        curl_easy_setopt(handle, CURLOPT_WRITEDATA, &result.body);
        curl_easy_setopt(handle, CURLOPT_CONNECTTIMEOUT_MS, 2000L);
        curl_easy_setopt(handle, CURLOPT_TIMEOUT_MS, 30000L);
        curl_easy_setopt(handle, CURLOPT_NOSIGNAL, 1L);
        if (method == "POST") {
            curl_easy_setopt(handle, CURLOPT_POST, 1L);
            curl_easy_setopt(handle, CURLOPT_POSTFIELDS, body->c_str());
            curl_easy_setopt(handle, CURLOPT_POSTFIELDSIZE, static_cast<long>(body->size()));
        }
        const CURLcode performed = curl_easy_perform(handle);
        curl_easy_getinfo(handle, CURLINFO_RESPONSE_CODE, &result.status);
        curl_slist_free_all(headers);
        curl_easy_cleanup(handle);
        if (performed != CURLE_OK) {
            throw std::runtime_error(
                std::string("service request failed: ") + curl_easy_strerror(performed));
        }
        json parsed;
        try {
            parsed = json::parse(result.body);
        } catch (const json::exception&) {
            throw std::runtime_error("service returned a non-JSON response");
        }
        if (result.status < 200 || result.status >= 300) {
            throw std::runtime_error(parsed.value("detail", parsed.value("code", "request failed")));
        }
        return parsed;
    }

    std::string endpoint_;
    std::string key_;
};

json command(
    const std::string& action,
    const std::string& kind,
    json parameters,
    const bool dry_run = false,
    const std::optional<std::string>& id = std::nullopt) {
    const std::string request_id = arena::domain::uuid_v7();
    return {
        {"schema_version", 1},
        {"request_id", request_id},
        {"idempotency_key", "cli-" + request_id},
        {"target", {{"kind", kind}, {"id", id ? json(*id) : json(nullptr)}}},
        {"action", action},
        {"expected_revision", nullptr},
        {"dry_run", dry_run},
        {"parameters", std::move(parameters)},
    };
}

void print_json(const json& value, const std::string& output) {
    if (output == "jsonl" && value.is_array()) {
        for (const auto& row : value) {
            std::cout << row.dump() << '\n';
        }
        return;
    }
    std::cout << value.dump(output == "json" || output == "jsonl" ? -1 : 2) << '\n';
}

void open_browser(const std::string& url) {
#ifdef _WIN32
    const int size = MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, url.data(), static_cast<int>(url.size()), nullptr, 0);
    std::wstring wide(static_cast<std::size_t>(size), L'\0');
    MultiByteToWideChar(
        CP_UTF8,
        MB_ERR_INVALID_CHARS,
        url.data(),
        static_cast<int>(url.size()),
        wide.data(),
        size);
    if (reinterpret_cast<std::intptr_t>(
            ShellExecuteW(nullptr, L"open", wide.c_str(), nullptr, nullptr, SW_SHOWNORMAL)) <= 32) {
        throw std::runtime_error("cannot open the default browser");
    }
#else
    const pid_t child = fork();
    if (child == 0) {
        execlp("xdg-open", "xdg-open", url.c_str(), static_cast<char*>(nullptr));
        _exit(127);
    }
    if (child < 0) {
        throw std::runtime_error("cannot open the default browser");
    }
#endif
}

}  // namespace

int main(int argc, char** argv) {
    curl_global_init(CURL_GLOBAL_DEFAULT);
    try {
        CLI::App app{"Native CLI for the UCI Arena Manager control plane"};
        app.require_subcommand(1);
        std::string endpoint{"http://127.0.0.1:47831"};
        std::filesystem::path data_directory{"data"};
        std::string output{"human"};
        app.add_option("--endpoint", endpoint, "Service endpoint");
        app.add_option("--data-dir", data_directory, "Service data directory");
        app.add_option("--output", output, "Output: human, json, or jsonl")
            ->check(CLI::IsMember({"human", "json", "jsonl"}));

        auto* status = app.add_subcommand("status", "Show control-plane status");
        auto* health = app.add_subcommand("health", "Show control-plane health");
        auto* doctor = app.add_subcommand("doctor", "Run safe local diagnostics");
        auto* ui = app.add_subcommand("ui", "Open the authenticated local dashboard");
        bool no_open = false;
        ui->add_flag("--no-open", no_open, "Print the URL without opening a browser");

        auto* daemon = app.add_subcommand("daemon", "Control the installed arena service");
        daemon->require_subcommand(1);
        auto* daemon_status = daemon->add_subcommand("status", "Query native service state");
        auto* daemon_start = daemon->add_subcommand("start", "Start the native service");
        auto* daemon_stop = daemon->add_subcommand("stop", "Stop the native service");
        auto* daemon_restart = daemon->add_subcommand("restart", "Restart the native service");

        auto* engine = app.add_subcommand("engine", "Manage UCI engines");
        engine->require_subcommand(1);
        auto* engine_list = engine->add_subcommand("list", "List registered engines");
        auto* engine_probe = engine->add_subcommand("probe", "Probe an engine without registering");
        std::filesystem::path probe_executable;
        std::filesystem::path probe_working_directory;
        std::vector<std::string> probe_arguments;
        engine_probe->add_option("executable", probe_executable)->required();
        engine_probe->add_option("--working-directory", probe_working_directory);
        engine_probe->add_option("--arg", probe_arguments);

        auto* engine_add = engine->add_subcommand("add", "Probe and register a UCI engine");
        std::string engine_name;
        std::filesystem::path engine_executable;
        std::filesystem::path engine_working_directory;
        std::vector<std::string> engine_arguments;
        bool engine_dry_run = false;
        engine_add->add_option("--name", engine_name)->required();
        engine_add->add_option("--executable", engine_executable)->required();
        engine_add->add_option("--working-directory", engine_working_directory);
        engine_add->add_option("--arg", engine_arguments);
        engine_add->add_flag("--dry-run", engine_dry_run);

        auto* match = app.add_subcommand("match", "Plan local engine matches");
        match->require_subcommand(1);
        auto* match_plan = match->add_subcommand(
            "plan", "Plan LC0, Stockfish, LatticeKnight, or any registered UCI engines");
        std::string match_name;
        std::vector<std::string> match_engines;
        std::string match_format{"head_to_head"};
        std::string match_variant{"standard"};
        std::uint32_t games_per_pair{2};
        std::uint32_t rounds{1};
        std::uint32_t concurrency{1};
        std::uint64_t initial_ms{60'000};
        std::uint64_t increment_ms{600};
        std::uint64_t seed{0};
        bool match_dry_run = false;
        match_plan->add_option("--name", match_name)->required();
        match_plan->add_option("--engine", match_engines)->required()->expected(-1);
        match_plan->add_option("--format", match_format)
            ->check(CLI::IsMember({"head_to_head", "round_robin", "gauntlet", "swiss", "sprt"}));
        match_plan->add_option("--variant", match_variant)
            ->check(CLI::IsMember({"standard", "chess960"}));
        match_plan->add_option("--games-per-pair", games_per_pair);
        match_plan->add_option("--rounds", rounds);
        match_plan->add_option("--concurrency", concurrency);
        match_plan->add_option("--initial-ms", initial_ms);
        match_plan->add_option("--increment-ms", increment_ms);
        match_plan->add_option("--seed", seed);
        match_plan->add_flag("--dry-run", match_dry_run);

        auto* tournament = app.add_subcommand("tournament", "Plan engine tournaments");
        tournament->require_subcommand(1);
        auto* tournament_create = tournament->add_subcommand("create", "Create a tournament plan");
        std::string tournament_name;
        std::vector<std::string> tournament_engines;
        std::string tournament_format{"round_robin"};
        std::uint32_t tournament_games{2};
        std::uint32_t tournament_rounds{1};
        std::uint32_t tournament_concurrency{1};
        std::uint64_t tournament_seed{0};
        tournament_create->add_option("--name", tournament_name)->required();
        tournament_create->add_option("--engine", tournament_engines)->required()->expected(-1);
        tournament_create->add_option("--format", tournament_format)
            ->check(CLI::IsMember({"round_robin", "gauntlet", "swiss", "sprt"}));
        tournament_create->add_option("--games-per-pair", tournament_games);
        tournament_create->add_option("--rounds", tournament_rounds);
        tournament_create->add_option("--concurrency", tournament_concurrency);
        tournament_create->add_option("--seed", tournament_seed);

        auto* resource = app.add_subcommand("resource", "Inspect capability-described resources");
        resource->require_subcommand(1);
        auto* resource_list = resource->add_subcommand("list", "List resources by kind");
        std::string resource_kind;
        resource_list->add_option("--kind", resource_kind)->required();

        auto* service = app.add_subcommand("service", "Manage external service adapters");
        service->require_subcommand(1);
        auto* service_profiles =
            service->add_subcommand("profiles", "List built-in capability profiles");
        auto* service_discover =
            service->add_subcommand("discover", "Detect local configuration hints");
        std::string service_discovery_profile;
        service_discover->add_option(
            "profile",
            service_discovery_profile,
            "book_forge or uci_arena_evidence")
            ->required()
            ->check(CLI::IsMember({"book_forge", "uci_arena_evidence"}));
        auto* service_list =
            service->add_subcommand("list", "List configured managed services");
        auto* service_book_forge =
            service->add_subcommand("add-book-forge", "Configure the Book Forge adapter");
        std::filesystem::path book_node;
        std::filesystem::path book_cli;
        std::filesystem::path book_workspace;
        service_book_forge->add_option("--node", book_node, "Node.js executable")->required();
        service_book_forge->add_option("--cli", book_cli, "Book Forge CLI entrypoint")->required();
        service_book_forge->add_option("--workspace", book_workspace)->required();
        auto* service_configure_book_forge = service->add_subcommand(
            "configure-book-forge",
            "Change the locator for a configured Book Forge service");
        std::string configure_book_id;
        std::filesystem::path configure_book_node;
        std::filesystem::path configure_book_cli;
        std::filesystem::path configure_book_workspace;
        service_configure_book_forge->add_option("id", configure_book_id)->required();
        service_configure_book_forge
            ->add_option("--node", configure_book_node, "Node.js executable")
            ->required();
        service_configure_book_forge
            ->add_option("--cli", configure_book_cli, "Book Forge CLI entrypoint")
            ->required();
        service_configure_book_forge
            ->add_option("--workspace", configure_book_workspace)
            ->required();
        auto* service_evidence = service->add_subcommand(
            "track-evidence-service",
            "Register the in-development UCI Arena Evidence Service");
        std::string evidence_endpoint;
        service_evidence->add_option(
            "--service-endpoint",
            evidence_endpoint,
            "Optional future Evidence Service base URL");
        auto* service_configure_evidence = service->add_subcommand(
            "configure-evidence-service",
            "Change the endpoint for a tracked UCI Arena Evidence Service");
        std::string configure_evidence_id;
        std::string configure_evidence_endpoint;
        service_configure_evidence->add_option("id", configure_evidence_id)->required();
        service_configure_evidence
            ->add_option(
                "--service-endpoint",
                configure_evidence_endpoint,
                "Loopback Evidence Service base URL")
            ->required();
        std::string service_control_id;
        std::string service_control_action;
        auto* service_control =
            service->add_subcommand("control", "Start, stop, restart, pause, resume, or probe");
        service_control->add_option("id", service_control_id)->required();
        service_control->add_option("action", service_control_action)
            ->required()
            ->check(CLI::IsMember({"start", "stop", "restart", "pause", "resume", "status"}));

        CLI11_PARSE(app, argc, argv);

        if (*engine_probe) {
            if (probe_working_directory.empty()) {
                probe_working_directory = probe_executable.parent_path();
            }
            const auto result = arena::adapters::UciProcess::probe(
                probe_executable, probe_working_directory, probe_arguments);
            print_json(
                {
                    {"ok", result.ok},
                    {"engine_name", result.engine_name},
                    {"engine_author", result.engine_author},
                    {"fingerprint", result.fingerprint},
                    {"bestmove", result.bestmove},
                    {"option_count", result.options.size()},
                    {"elapsed_ms", result.elapsed_ms},
                    {"error", result.error},
                },
                output);
            curl_global_cleanup();
            return result.ok ? 0 : 3;
        }

        if (*daemon_status || *daemon_start || *daemon_stop || *daemon_restart) {
            const std::string action =
                *daemon_start ? "start" :
                *daemon_stop ? "stop" :
                *daemon_restart ? "restart" : "status";
#ifdef _WIN32
            const std::string platform_name = "UCIArenaManager";
#else
            const std::string platform_name = "uci-arena-manager.service";
#endif
            const int result =
                arena::application::perform_service_action(platform_name, action);
            print_json(
                {
                    {"ok", result == 0 || action == "status"},
                    {"service", platform_name},
                    {"action", action},
                    {"platform_state", result},
                },
                output);
            curl_global_cleanup();
            return 0;
        }

        HttpClient client(endpoint, data_directory / "client.key");
        if (*status || *health) {
            print_json(client.get("/api/v1/system/status"), output);
        } else if (*doctor) {
            const auto state = client.get("/api/v1/system/status");
            print_json(
                {
                    {"ok", true},
                    {"service", state},
                    {"client_key", (data_directory / "client.key").string()},
                    {"checks", {{"authentication", "ok"}, {"database", "ok"}, {"loopback", "ok"}}},
                },
                output);
        } else if (*ui) {
            const auto pairing = client.post("/api/v1/auth/pair-token", json::object());
            const std::string url =
                endpoint + "/#pair=" + pairing.at("pairing_token").get<std::string>();
            std::cout << url << '\n';
            if (!no_open) {
                open_browser(url);
            }
        } else if (*engine_list) {
            print_json(client.get("/api/v1/resources/engine"), output);
        } else if (*engine_add) {
            if (engine_working_directory.empty()) {
                engine_working_directory = engine_executable.parent_path();
            }
            print_json(
                client.post(
                    "/api/v1/commands",
                    command(
                        "engine.register",
                        "engine",
                        {
                            {"display_name", engine_name},
                            {"executable", engine_executable.string()},
                            {"working_directory", engine_working_directory.string()},
                            {"arguments", engine_arguments},
                        },
                        engine_dry_run)),
                output);
        } else if (*match_plan) {
            print_json(
                client.post(
                    "/api/v1/commands",
                    command(
                        "match.plan",
                        "match_plan",
                        {
                            {"display_name", match_name},
                            {"engine_ids", match_engines},
                            {"format", match_format},
                            {"variant", match_variant},
                            {"games_per_pair", games_per_pair},
                            {"rounds", rounds},
                            {"concurrency", concurrency},
                            {"seed", seed},
                            {"time_control",
                             {{"initial_ms", initial_ms}, {"increment_ms", increment_ms}}},
                        },
                        match_dry_run)),
                output);
        } else if (*tournament_create) {
            print_json(
                client.post(
                    "/api/v1/commands",
                    command(
                        "tournament.plan",
                        "tournament",
                        {
                            {"display_name", tournament_name},
                            {"engine_ids", tournament_engines},
                            {"format", tournament_format},
                            {"games_per_pair", tournament_games},
                            {"rounds", tournament_rounds},
                            {"concurrency", tournament_concurrency},
                            {"seed", tournament_seed},
                        })),
                output);
        } else if (*resource_list) {
            print_json(client.get("/api/v1/resources/" + resource_kind), output);
        } else if (*service_profiles) {
            print_json(client.get("/api/v1/service-profiles"), output);
        } else if (*service_discover) {
            print_json(
                client.get(
                    "/api/v1/service-profiles/" + service_discovery_profile + "/discovery"),
                output);
        } else if (*service_list) {
            print_json(client.get("/api/v1/resources/managed_service"), output);
        } else if (*service_book_forge) {
            print_json(
                client.post(
                    "/api/v1/commands",
                    command(
                        "service.register",
                        "managed_service",
                        {
                            {"profile_id", "book_forge"},
                            {"display_name", "Book Forge"},
                            {"node_executable", book_node.string()},
                            {"cli_entrypoint", book_cli.string()},
                            {"workspace", book_workspace.string()},
                        })),
                output);
        } else if (*service_configure_book_forge) {
            auto envelope = command(
                "service.configure",
                "managed_service",
                {
                    {"profile_id", "book_forge"},
                    {"display_name", "Book Forge"},
                    {"node_executable", configure_book_node.string()},
                    {"cli_entrypoint", configure_book_cli.string()},
                    {"workspace", configure_book_workspace.string()},
                });
            envelope["target"]["id"] = configure_book_id;
            print_json(client.post("/api/v1/commands", envelope), output);
        } else if (*service_evidence) {
            print_json(
                client.post(
                    "/api/v1/commands",
                    command(
                        "service.register",
                        "managed_service",
                        {
                            {"profile_id", "uci_arena_evidence"},
                            {"display_name", "UCI Arena Evidence Service"},
                            {"endpoint", evidence_endpoint},
                        })),
                output);
        } else if (*service_configure_evidence) {
            auto envelope = command(
                "service.configure",
                "managed_service",
                {
                    {"profile_id", "uci_arena_evidence"},
                    {"display_name", "UCI Arena Evidence Service"},
                    {"endpoint", configure_evidence_endpoint},
                });
            envelope["target"]["id"] = configure_evidence_id;
            print_json(client.post("/api/v1/commands", envelope), output);
        } else if (*service_control) {
            auto envelope = command(
                "service." + service_control_action,
                "managed_service",
                {});
            envelope["target"]["id"] = service_control_id;
            print_json(client.post("/api/v1/commands", envelope), output);
        }
        curl_global_cleanup();
        return 0;
    } catch (const CLI::ParseError& error) {
        curl_global_cleanup();
        return error.get_exit_code();
    } catch (const std::exception& error) {
        std::cerr << "arena: " << error.what() << '\n';
        curl_global_cleanup();
        return 2;
    }
}
