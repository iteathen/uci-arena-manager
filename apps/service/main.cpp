#include "events_websocket.hpp"

#include "arena/adapters/sqlite_store.hpp"
#include "arena/application/control_plane.hpp"
#include "arena/application/engine_onboarding.hpp"
#include "arena/application/local_auth.hpp"
#include "arena/domain/uuid.hpp"

#include <drogon/drogon.h>
#include <nlohmann/json.hpp>

#include <charconv>
#include <filesystem>
#include <iostream>
#include <memory>
#include <string>

#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#include <winsvc.h>
#endif

namespace {

using drogon::HttpRequestPtr;
using drogon::HttpResponse;
using drogon::HttpResponsePtr;
using nlohmann::json;

struct Options {
    std::filesystem::path data_directory{"data"};
    std::filesystem::path web_root{"web/dist"};
    std::uint16_t port{47831};
    bool windows_service{false};
};

Options parse_options(int argc, char** argv) {
    Options options;
    for (int index = 1; index < argc; ++index) {
        const std::string argument = argv[index];
        if (argument == "--data-dir" && index + 1 < argc) {
            options.data_directory = argv[++index];
        } else if (argument == "--web-root" && index + 1 < argc) {
            options.web_root = argv[++index];
        } else if (argument == "--port" && index + 1 < argc) {
            unsigned int port = 0;
            const std::string value = argv[++index];
            const auto parsed = std::from_chars(value.data(), value.data() + value.size(), port);
            if (parsed.ec != std::errc{} || port == 0 || port > 65535) {
                throw std::runtime_error("invalid --port value");
            }
            options.port = static_cast<std::uint16_t>(port);
        } else if (argument == "--windows-service") {
            options.windows_service = true;
        } else if (argument == "--help") {
            std::cout << "arena-service [--data-dir PATH] [--web-root PATH] [--port PORT]"
                         " [--windows-service]\n";
            std::exit(0);
        } else {
            throw std::runtime_error("unknown argument: " + argument);
        }
    }
    return options;
}

HttpResponsePtr response(const json& body, drogon::HttpStatusCode status = drogon::k200OK) {
    auto result = HttpResponse::newHttpResponse();
    result->setStatusCode(status);
    result->setContentTypeCode(drogon::CT_APPLICATION_JSON);
    result->setBody(body.dump());
    result->addHeader("Cache-Control", "no-store");
    result->addHeader("X-Content-Type-Options", "nosniff");
    result->addHeader("Content-Security-Policy", "default-src 'self'; connect-src 'self' ws://127.0.0.1:*; "
                                                "style-src 'self' 'unsafe-inline'; img-src 'self' data:");
    return result;
}

bool authorized(
    const HttpRequestPtr& request,
    const std::shared_ptr<arena::application::LocalAuth>& auth) {
    if (auth->authenticate_bearer(request->getHeader("authorization"))) {
        return true;
    }
    return auth->authenticate_session(request->getCookie("arena_session"));
}

bool mutation_authorized(
    const HttpRequestPtr& request,
    const std::shared_ptr<arena::application::LocalAuth>& auth) {
    if (auth->authenticate_bearer(request->getHeader("authorization"))) {
        return true;
    }
    const auto origin = request->getHeader("origin");
    if (origin.empty() ||
        (!origin.starts_with("http://127.0.0.1:") &&
         !origin.starts_with("http://localhost:"))) {
        return false;
    }
    const auto session = request->getCookie("arena_session");
    return auth->validate_csrf(session, request->getHeader("x-arena-csrf"));
}

json parse_body(const HttpRequestPtr& request) {
    if (request->body().size() > 1024 * 1024) {
        throw arena::application::CommandFailure(
            "request_too_large", "request body exceeds one MiB", 413);
    }
    try {
        return json::parse(request->body());
    } catch (const json::exception&) {
        throw arena::application::CommandFailure(
            "invalid_json", "request body is not valid JSON", 400);
    }
}

drogon::HttpStatusCode status_code(const int status) {
    return static_cast<drogon::HttpStatusCode>(status);
}

int run_control_plane(const Options& options) {
    try {
        std::filesystem::create_directories(options.data_directory);
        auto store = std::make_shared<arena::adapters::SqliteStore>(
            options.data_directory / "arena.db");
        auto control_plane = std::make_shared<arena::application::ControlPlane>(store);
        control_plane->initialize();
        control_plane->reconcile_installed_services(options.data_directory);
        auto auth = std::make_shared<arena::application::LocalAuth>(options.data_directory);
        auth->initialize();
        arena::service::EventsWebSocket::configure(control_plane, auth);

        auto& app = drogon::app();
        app.addListener("127.0.0.1", options.port)
            .setThreadNum(4)
            .setDocumentRoot(options.web_root.string())
            .setFileTypes({"html", "js", "css", "svg", "png", "ico", "woff2"});

        app.registerHandler(
            "/api/v1/auth/pair-token",
            [auth](const HttpRequestPtr& request,
                   std::function<void(const HttpResponsePtr&)>&& callback) {
                if (!auth->authenticate_bearer(request->getHeader("authorization"))) {
                    callback(response(
                        {{"code", "authentication_required"}},
                        drogon::k401Unauthorized));
                    return;
                }
                callback(response(
                    {{"pairing_token", auth->issue_pairing_token()}, {"expires_in_seconds", 60}}));
            },
            {drogon::Post});

        app.registerHandler(
            "/api/v1/auth/pair",
            [auth](const HttpRequestPtr& request,
                   std::function<void(const HttpResponsePtr&)>&& callback) {
                try {
                    const auto session =
                        auth->exchange_pairing_token(parse_body(request).value("token", ""));
                    if (!session) {
                        callback(response(
                            {{"code", "invalid_pairing_token"}},
                            drogon::k401Unauthorized));
                        return;
                    }
                    auto result = response({{"csrf_token", session->csrf_token}});
                    drogon::Cookie cookie("arena_session", session->id);
                    cookie.setHttpOnly(true);
                    cookie.setSameSite(drogon::Cookie::SameSite::kStrict);
                    cookie.setPath("/");
                    cookie.setMaxAge(43'200);
                    result->addCookie(std::move(cookie));
                    callback(result);
                } catch (const arena::application::CommandFailure& failure) {
                    callback(response(
                        arena::application::problem_json(failure),
                        status_code(failure.http_status())));
                }
            },
            {drogon::Post});

        app.registerHandler(
            "/api/v1/system/status",
            [control_plane, auth](
                const HttpRequestPtr& request,
                std::function<void(const HttpResponsePtr&)>&& callback) {
                if (!authorized(request, auth)) {
                    callback(response(
                        {{"code", "authentication_required"}},
                        drogon::k401Unauthorized));
                    return;
                }
                callback(response(control_plane->status()));
            },
            {drogon::Get});

        const auto register_resource_alias =
            [&app, control_plane, auth](const std::string& path, const std::string& kind) {
                app.registerHandler(
                    path,
                    [control_plane, auth, kind](
                        const HttpRequestPtr& request,
                        std::function<void(const HttpResponsePtr&)>&& callback) {
                        if (!authorized(request, auth)) {
                            callback(response(
                                {{"code", "authentication_required"}},
                                drogon::k401Unauthorized));
                            return;
                        }
                        callback(response(control_plane->list(kind)));
                    },
                    {drogon::Get});
            };
        register_resource_alias("/api/v1/engines", "engine");
        register_resource_alias("/api/v1/match-plans", "match_plan");
        register_resource_alias("/api/v1/tournaments", "tournament");

        app.registerHandler(
            "/api/v1/engine-candidates",
            [auth, data_directory = options.data_directory](
                const HttpRequestPtr& request,
                std::function<void(const HttpResponsePtr&)>&& callback) {
                if (!authorized(request, auth)) {
                    callback(response(
                        {{"code", "authentication_required"}},
                        drogon::k401Unauthorized));
                    return;
                }
                try {
                    std::vector<std::filesystem::path> roots;
                    const std::string root = request->getParameter("root");
                    if (!root.empty()) {
                        roots.emplace_back(root);
                    }
                    callback(response(
                        arena::application::EngineOnboarding::discover(
                            data_directory, roots)));
                } catch (const std::exception& error) {
                    callback(response(
                        {{"code", "engine_discovery_failed"},
                         {"detail", error.what()}},
                        drogon::k422UnprocessableEntity));
                }
            },
            {drogon::Get});

        app.registerHandler(
            "/api/v1/engine-probes",
            [control_plane, auth](
                const HttpRequestPtr& request,
                std::function<void(const HttpResponsePtr&)>&& callback) {
                if (!mutation_authorized(request, auth)) {
                    callback(response(
                        {{"code", "authentication_required"}},
                        drogon::k401Unauthorized));
                    return;
                }
                const std::string probe_id = arena::domain::uuid_v7();
                try {
                    const json body = parse_body(request);
                    json parameters = json::object();
                    for (const auto* field :
                         {"display_name", "executable", "working_directory",
                          "arguments", "uci_options",
                          "environment_references"}) {
                        if (body.contains(field)) {
                            parameters[field] = body.at(field);
                        }
                    }
                    const json command{
                        {"schema_version", 1},
                        {"request_id", arena::domain::uuid_v7()},
                        {"idempotency_key",
                         "engine-probe-" + arena::domain::uuid_v7()},
                        {"target", {{"kind", "engine"}, {"id", nullptr}}},
                        {"action", "engine.register"},
                        {"expected_revision", nullptr},
                        {"dry_run", true},
                        {"parameters", parameters},
                    };
                    const json preview =
                        control_plane->submit(command, "local-admin");
                    callback(response({
                        {"schema", "arena_engine_probe_result_v1"},
                        {"schema_version", 1},
                        {"probe_id", probe_id},
                        {"candidate_id", body.value("candidate_id", "")},
                        {"state", "passed"},
                        {"registration_parameters", parameters},
                        {"preview", preview},
                    }));
                } catch (const arena::application::CommandFailure& failure) {
                    callback(response(
                        {
                            {"schema", "arena_engine_probe_result_v1"},
                            {"schema_version", 1},
                            {"probe_id", probe_id},
                            {"candidate_id", ""},
                            {"state", "failed"},
                            {"registration_parameters", json::object()},
                            {"diagnostic",
                             arena::application::problem_json(failure)},
                        },
                        status_code(failure.http_status())));
                }
            },
            {drogon::Post});

        app.registerHandler(
            "/api/v1/resources/{1}",
            [control_plane, auth](
                const HttpRequestPtr& request,
                std::function<void(const HttpResponsePtr&)>&& callback,
                const std::string& kind) {
                if (!authorized(request, auth)) {
                    callback(response(
                        {{"code", "authentication_required"}},
                        drogon::k401Unauthorized));
                    return;
                }
                std::uint32_t limit = 200;
                std::uint32_t offset = 0;
                try {
                    if (!request->getParameter("limit").empty()) {
                        limit = static_cast<std::uint32_t>(
                            std::stoul(request->getParameter("limit")));
                    }
                    if (!request->getParameter("offset").empty()) {
                        offset = static_cast<std::uint32_t>(
                            std::stoul(request->getParameter("offset")));
                    }
                    callback(response(control_plane->list(kind, limit, offset)));
                } catch (const std::exception&) {
                    callback(response({{"code", "invalid_pagination"}}, drogon::k400BadRequest));
                }
            },
            {drogon::Get});

        app.registerHandler(
            "/api/v1/service-profiles",
            [control_plane, auth](
                const HttpRequestPtr& request,
                std::function<void(const HttpResponsePtr&)>&& callback) {
                if (!authorized(request, auth)) {
                    callback(response(
                        {{"code", "authentication_required"}},
                        drogon::k401Unauthorized));
                    return;
                }
                callback(response(control_plane->service_profiles()));
            },
            {drogon::Get});

        app.registerHandler(
            "/api/v1/service-profiles/{1}/discovery",
            [control_plane, auth](
                const HttpRequestPtr& request,
                std::function<void(const HttpResponsePtr&)>&& callback,
                const std::string& profile_id) {
                if (!authorized(request, auth)) {
                    callback(response(
                        {{"code", "authentication_required"}},
                        drogon::k401Unauthorized));
                    return;
                }
                try {
                    callback(response(control_plane->discover_service_profile(profile_id)));
                } catch (const arena::application::CommandFailure& failure) {
                    callback(response(
                        arena::application::problem_json(failure),
                        status_code(failure.http_status())));
                }
            },
            {drogon::Get});

        app.registerHandler(
            "/api/v1/events/snapshot",
            [control_plane, auth](
                const HttpRequestPtr& request,
                std::function<void(const HttpResponsePtr&)>&& callback) {
                if (!authorized(request, auth)) {
                    callback(response(
                        {{"code", "authentication_required"}},
                        drogon::k401Unauthorized));
                    return;
                }
                std::uint64_t cursor = 0;
                try {
                    if (!request->getParameter("cursor").empty()) {
                        cursor = std::stoull(request->getParameter("cursor"));
                    }
                    callback(response(control_plane->events_after(cursor)));
                } catch (const std::exception&) {
                    callback(response({{"code", "invalid_cursor"}}, drogon::k400BadRequest));
                }
            },
            {drogon::Get});

        app.registerHandler(
            "/api/v1/commands",
            [control_plane, auth](
                const HttpRequestPtr& request,
                std::function<void(const HttpResponsePtr&)>&& callback) {
                if (!mutation_authorized(request, auth)) {
                    callback(response(
                        {{"code", "authentication_required"}},
                        drogon::k401Unauthorized));
                    return;
                }
                try {
                    callback(response(control_plane->submit(parse_body(request), "local-admin")));
                } catch (const arena::application::CommandFailure& failure) {
                    callback(response(
                        arena::application::problem_json(failure),
                        status_code(failure.http_status())));
                }
            },
            {drogon::Post});

        std::cout << "UCI Arena Manager listening on http://127.0.0.1:" << options.port
                  << "\nClient key: " << auth->client_key_path().string() << '\n';
        app.run();
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "arena-service: " << error.what() << '\n';
        return 1;
    }
}

#ifdef _WIN32
Options service_options;
SERVICE_STATUS_HANDLE service_status_handle = nullptr;
SERVICE_STATUS service_status{
    .dwServiceType = SERVICE_WIN32_OWN_PROCESS,
    .dwCurrentState = SERVICE_START_PENDING,
    .dwControlsAccepted = 0,
    .dwWin32ExitCode = NO_ERROR,
    .dwServiceSpecificExitCode = 0,
    .dwCheckPoint = 0,
    .dwWaitHint = 10'000,
};

void WINAPI service_control_handler(const DWORD control) {
    if (control == SERVICE_CONTROL_STOP || control == SERVICE_CONTROL_SHUTDOWN) {
        service_status.dwCurrentState = SERVICE_STOP_PENDING;
        service_status.dwControlsAccepted = 0;
        service_status.dwWaitHint = 10'000;
        SetServiceStatus(service_status_handle, &service_status);
        drogon::app().quit();
    } else if (control == SERVICE_CONTROL_INTERROGATE) {
        SetServiceStatus(service_status_handle, &service_status);
    }
}

void WINAPI service_entry(DWORD argc, wchar_t** argv) {
    (void)argc;
    (void)argv;
    service_status_handle =
        RegisterServiceCtrlHandlerW(L"UCIArenaManager", service_control_handler);
    if (service_status_handle == nullptr) {
        return;
    }
    service_status.dwCurrentState = SERVICE_RUNNING;
    service_status.dwControlsAccepted = SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_SHUTDOWN;
    service_status.dwWaitHint = 0;
    SetServiceStatus(service_status_handle, &service_status);

    const int result = run_control_plane(service_options);
    service_status.dwCurrentState = SERVICE_STOPPED;
    service_status.dwControlsAccepted = 0;
    service_status.dwWin32ExitCode = result == 0 ? NO_ERROR : ERROR_SERVICE_SPECIFIC_ERROR;
    service_status.dwServiceSpecificExitCode = static_cast<DWORD>(result);
    SetServiceStatus(service_status_handle, &service_status);
}

int run_windows_service(const Options& options) {
    service_options = options;
    SERVICE_TABLE_ENTRYW table[] = {
        {const_cast<wchar_t*>(L"UCIArenaManager"), service_entry},
        {nullptr, nullptr},
    };
    if (!StartServiceCtrlDispatcherW(table)) {
        std::cerr << "arena-service: cannot connect to Service Control Manager, error "
                  << GetLastError() << '\n';
        return 1;
    }
    return 0;
}
#endif

}  // namespace

int main(int argc, char** argv) {
    try {
        const Options options = parse_options(argc, argv);
#ifdef _WIN32
        if (options.windows_service) {
            return run_windows_service(options);
        }
#endif
        return run_control_plane(options);
    } catch (const std::exception& error) {
        std::cerr << "arena-service: " << error.what() << '\n';
        return 1;
    }
}
