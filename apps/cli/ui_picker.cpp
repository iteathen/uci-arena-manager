#include "ui_picker.hpp"

#include "arena/application/process_supervisor.hpp"
#include "arena/application/service_catalog.hpp"

#include <drogon/drogon.h>
#include <nlohmann/json.hpp>
#include <sodium.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <filesystem>
#include <functional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#ifdef _WIN32
#define NOMINMAX
#include <shobjidl.h>
#include <windows.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace arena::cli {
namespace {

using nlohmann::json;

struct PickerFilter {
    std::string name;
    std::vector<std::string> patterns;
};

struct PickerRequest {
    bool directory{};
    std::string title;
    std::vector<PickerFilter> filters;
};

json problem(const std::string& code, const std::string& detail) {
    return {
        {"schema", "arena_ui_picker_result_v1"},
        {"status", "error"},
        {"code", code},
        {"detail", detail},
    };
}

void add_cors_headers(
    const drogon::HttpResponsePtr& response,
    const std::string& dashboard_origin) {
    response->addHeader("Access-Control-Allow-Origin", dashboard_origin);
    response->addHeader("Access-Control-Allow-Headers", "authorization,content-type");
    response->addHeader("Access-Control-Allow-Methods", "POST,OPTIONS");
    response->addHeader("Access-Control-Allow-Private-Network", "true");
    response->addHeader("Cache-Control", "no-store");
    response->addHeader("Vary", "Origin");
}

drogon::HttpResponsePtr json_response(
    const json& body,
    const drogon::HttpStatusCode status,
    const std::string& dashboard_origin) {
    auto response = drogon::HttpResponse::newHttpResponse();
    response->setContentTypeCode(drogon::CT_APPLICATION_JSON);
    response->setBody(body.dump());
    response->setStatusCode(status);
    add_cors_headers(response, dashboard_origin);
    return response;
}

PickerRequest parse_request(const drogon::HttpRequestPtr& request) {
    const auto input = json::parse(request->body());
    if (!input.is_object() || input.value("schema", "") != "arena_ui_picker_request_v1") {
        throw std::invalid_argument("schema must be arena_ui_picker_request_v1");
    }

    const std::string mode = input.value("mode", "");
    if (mode != "file" && mode != "directory") {
        throw std::invalid_argument("mode must be file or directory");
    }

    PickerRequest result;
    result.directory = mode == "directory";
    result.title = input.value("title", result.directory ? "Select directory" : "Select file");
    if (result.title.empty() || result.title.size() > 160) {
        throw std::invalid_argument("title must contain 1 through 160 characters");
    }

    if (input.contains("filters")) {
        if (!input.at("filters").is_array() || input.at("filters").size() > 8) {
            throw std::invalid_argument("filters must be an array with at most 8 entries");
        }
        for (const auto& value : input.at("filters")) {
            PickerFilter filter;
            filter.name = value.value("name", "");
            if (filter.name.empty() || filter.name.size() > 80 ||
                !value.contains("patterns") || !value.at("patterns").is_array() ||
                value.at("patterns").empty() || value.at("patterns").size() > 8) {
                throw std::invalid_argument("each filter needs a short name and 1 through 8 patterns");
            }
            for (const auto& pattern : value.at("patterns")) {
                const auto text = pattern.get<std::string>();
                if (text.empty() || text.size() > 32 ||
                    !std::all_of(text.begin(), text.end(), [](const unsigned char character) {
                        return std::isalnum(character) || character == '*' || character == '?' ||
                               character == '.' || character == '-' || character == '_';
                    })) {
                    throw std::invalid_argument("filter patterns contain unsupported characters");
                }
                filter.patterns.push_back(text);
            }
            result.filters.push_back(std::move(filter));
        }
    }
    return result;
}

#ifdef _WIN32
std::wstring widen(const std::string& value) {
    if (value.empty()) {
        return {};
    }
    const int count = MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), nullptr, 0);
    if (count <= 0) {
        throw std::runtime_error("picker text is not valid UTF-8");
    }
    std::wstring result(static_cast<std::size_t>(count), L'\0');
    MultiByteToWideChar(
        CP_UTF8,
        MB_ERR_INVALID_CHARS,
        value.data(),
        static_cast<int>(value.size()),
        result.data(),
        count);
    return result;
}

std::string narrow(const wchar_t* value) {
    const int count = WideCharToMultiByte(
        CP_UTF8, WC_ERR_INVALID_CHARS, value, -1, nullptr, 0, nullptr, nullptr);
    if (count <= 1) {
        return {};
    }
    std::string result(static_cast<std::size_t>(count), '\0');
    WideCharToMultiByte(
        CP_UTF8,
        WC_ERR_INVALID_CHARS,
        value,
        -1,
        result.data(),
        count,
        nullptr,
        nullptr);
    result.resize(static_cast<std::size_t>(count - 1));
    return result;
}

json select_path(const PickerRequest& request) {
    const HRESULT initialized = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(initialized) && initialized != RPC_E_CHANGED_MODE) {
        return problem("picker_unavailable", "Windows common item dialog initialization failed");
    }

    IFileOpenDialog* dialog = nullptr;
    HRESULT status = CoCreateInstance(
        CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog));
    if (FAILED(status) || dialog == nullptr) {
        if (SUCCEEDED(initialized)) {
            CoUninitialize();
        }
        return problem("picker_unavailable", "Windows common item dialog is unavailable");
    }

    DWORD options = 0;
    dialog->GetOptions(&options);
    options |= FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST;
    if (request.directory) {
        options |= FOS_PICKFOLDERS;
    } else {
        options |= FOS_FILEMUSTEXIST;
    }
    dialog->SetOptions(options);
    const auto title = widen(request.title);
    dialog->SetTitle(title.c_str());

    std::vector<std::wstring> names;
    std::vector<std::wstring> patterns;
    std::vector<COMDLG_FILTERSPEC> native_filters;
    if (!request.directory) {
        names.reserve(request.filters.size());
        patterns.reserve(request.filters.size());
        native_filters.reserve(request.filters.size());
        for (const auto& filter : request.filters) {
            names.push_back(widen(filter.name));
            std::string joined;
            for (const auto& pattern : filter.patterns) {
                if (!joined.empty()) {
                    joined.push_back(';');
                }
                joined += pattern;
            }
            patterns.push_back(widen(joined));
        }
        for (std::size_t index = 0; index < names.size(); ++index) {
            native_filters.push_back({names[index].c_str(), patterns[index].c_str()});
        }
        if (!native_filters.empty()) {
            dialog->SetFileTypes(
                static_cast<UINT>(native_filters.size()), native_filters.data());
        }
    }

    status = dialog->Show(nullptr);
    if (status == HRESULT_FROM_WIN32(ERROR_CANCELLED)) {
        dialog->Release();
        if (SUCCEEDED(initialized)) {
            CoUninitialize();
        }
        return {
            {"schema", "arena_ui_picker_result_v1"},
            {"status", "cancelled"},
            {"path", nullptr},
        };
    }
    if (FAILED(status)) {
        dialog->Release();
        if (SUCCEEDED(initialized)) {
            CoUninitialize();
        }
        return problem("picker_failed", "Windows common item dialog failed");
    }

    IShellItem* selected = nullptr;
    status = dialog->GetResult(&selected);
    PWSTR raw_path = nullptr;
    if (SUCCEEDED(status) && selected != nullptr) {
        status = selected->GetDisplayName(SIGDN_FILESYSPATH, &raw_path);
    }
    std::string path;
    if (SUCCEEDED(status) && raw_path != nullptr) {
        path = narrow(raw_path);
    }
    if (raw_path != nullptr) {
        CoTaskMemFree(raw_path);
    }
    if (selected != nullptr) {
        selected->Release();
    }
    dialog->Release();
    if (SUCCEEDED(initialized)) {
        CoUninitialize();
    }
    if (path.empty()) {
        return problem("picker_failed", "The selected item has no local filesystem path");
    }
    return {
        {"schema", "arena_ui_picker_result_v1"},
        {"status", "selected"},
        {"path", path},
    };
}
#else
struct ProcessResult {
    int status{-1};
    std::string output;
};

ProcessResult run_picker_process(const std::vector<std::string>& arguments) {
    std::array<int, 2> output_pipe{};
    if (pipe(output_pipe.data()) != 0) {
        return {};
    }
    const pid_t child = fork();
    if (child == 0) {
        close(output_pipe[0]);
        dup2(output_pipe[1], STDOUT_FILENO);
        close(output_pipe[1]);
        const int null_fd = open("/dev/null", O_WRONLY);
        if (null_fd >= 0) {
            dup2(null_fd, STDERR_FILENO);
            close(null_fd);
        }
        std::vector<char*> argv;
        argv.reserve(arguments.size() + 1);
        for (const auto& argument : arguments) {
            argv.push_back(const_cast<char*>(argument.c_str()));
        }
        argv.push_back(nullptr);
        execvp(argv.front(), argv.data());
        _exit(errno == ENOENT ? 127 : 126);
    }
    close(output_pipe[1]);
    if (child < 0) {
        close(output_pipe[0]);
        return {};
    }

    ProcessResult result;
    std::array<char, 4096> buffer{};
    for (;;) {
        const ssize_t count = read(output_pipe[0], buffer.data(), buffer.size());
        if (count <= 0) {
            break;
        }
        if (result.output.size() + static_cast<std::size_t>(count) > 65536) {
            result.output.clear();
            break;
        }
        result.output.append(buffer.data(), static_cast<std::size_t>(count));
    }
    close(output_pipe[0]);
    int child_status = 0;
    if (waitpid(child, &child_status, 0) == child && WIFEXITED(child_status)) {
        result.status = WEXITSTATUS(child_status);
    }
    while (!result.output.empty() &&
           (result.output.back() == '\n' || result.output.back() == '\r')) {
        result.output.pop_back();
    }
    return result;
}

json select_path(const PickerRequest& request) {
    std::vector<std::string> arguments{
        "zenity",
        "--file-selection",
        "--title=" + request.title,
    };
    if (request.directory) {
        arguments.push_back("--directory");
    } else {
        for (const auto& filter : request.filters) {
            std::string value = "--file-filter=" + filter.name + " |";
            for (const auto& pattern : filter.patterns) {
                value += " " + pattern;
            }
            arguments.push_back(std::move(value));
        }
    }
    const auto selected = run_picker_process(arguments);
    if (selected.status == 0 && !selected.output.empty()) {
        return {
            {"schema", "arena_ui_picker_result_v1"},
            {"status", "selected"},
            {"path", selected.output},
        };
    }
    if (selected.status == 1) {
        return {
            {"schema", "arena_ui_picker_result_v1"},
            {"status", "cancelled"},
            {"path", nullptr},
        };
    }
    return problem(
        "picker_unavailable",
        "Install zenity for desktop browse dialogs, or enter the local path manually");
}
#endif

}  // namespace

std::string make_ui_picker_token() {
    if (sodium_init() < 0) {
        throw std::runtime_error("cannot initialize secure random generator");
    }
    std::array<unsigned char, 32> bytes{};
    randombytes_buf(bytes.data(), bytes.size());
    static constexpr std::string_view digits{"0123456789abcdef"};
    std::string result;
    result.reserve(bytes.size() * 2);
    for (const auto byte : bytes) {
        result.push_back(digits[byte >> 4U]);
        result.push_back(digits[byte & 0x0fU]);
    }
    return result;
}

void run_ui_picker(
    const std::uint16_t port,
    const std::string& bearer_token,
    const std::string& dashboard_origin,
    const std::filesystem::path& manager_data_root) {
    auto& application = drogon::app();
    application.registerHandler(
        "/v1/pick",
        [bearer_token, dashboard_origin](
            const drogon::HttpRequestPtr& request,
            std::function<void(const drogon::HttpResponsePtr&)>&& callback) {
            if (request->method() == drogon::Options) {
                const auto origin = request->getHeader("Origin");
                if (origin != dashboard_origin) {
                    callback(json_response(
                        problem("origin_rejected", "Dashboard origin is not authorized"),
                        drogon::k403Forbidden,
                        dashboard_origin));
                    return;
                }
                callback(json_response(json::object(), drogon::k204NoContent, dashboard_origin));
                return;
            }
            if (request->getHeader("Origin") != dashboard_origin ||
                request->getHeader("Authorization") != "Bearer " + bearer_token) {
                callback(json_response(
                    problem("picker_unauthorized", "Picker session is unavailable"),
                    drogon::k401Unauthorized,
                    dashboard_origin));
                return;
            }
            try {
                callback(json_response(
                    select_path(parse_request(request)),
                    drogon::k200OK,
                    dashboard_origin));
            } catch (const std::exception& error) {
                callback(json_response(
                    problem("invalid_picker_request", error.what()),
                    drogon::k400BadRequest,
                    dashboard_origin));
            }
        },
        {drogon::Post, drogon::Options});
    application.registerHandler(
        "/v1/bot-probe",
        [bearer_token, dashboard_origin, manager_data_root](
            const drogon::HttpRequestPtr& request,
            std::function<void(const drogon::HttpResponsePtr&)>&& callback) {
            if (request->method() == drogon::Options) {
                if (request->getHeader("Origin") != dashboard_origin) {
                    callback(json_response(
                        problem("origin_rejected", "Dashboard origin is not authorized"),
                        drogon::k403Forbidden,
                        dashboard_origin));
                    return;
                }
                callback(json_response(
                    json::object(), drogon::k204NoContent, dashboard_origin));
                return;
            }
            if (request->getHeader("Origin") != dashboard_origin ||
                request->getHeader("Authorization") !=
                    "Bearer " + bearer_token) {
                callback(json_response(
                    problem(
                        "picker_unauthorized",
                        "Interactive-user helper session is unavailable"),
                    drogon::k401Unauthorized,
                    dashboard_origin));
                return;
            }
            try {
                if (request->body().size() > 1024) {
                    throw std::invalid_argument("helper request is too large");
                }
                const json input = json::parse(request->body());
                if (!input.is_object() || input.size() != 1 ||
                    input.value("schema", "") !=
                        "arena_ui_bot_probe_request_v1") {
                    throw std::invalid_argument(
                        "schema must be arena_ui_bot_probe_request_v1");
                }
                arena::application::ProcessSupervisor process;
                callback(json_response(
                    arena::application::ServiceCatalog::probe_bot(
                        manager_data_root, process),
                    drogon::k200OK,
                    dashboard_origin));
            } catch (const std::invalid_argument&) {
                callback(json_response(
                    problem(
                        "invalid_bot_probe_request",
                        "Interactive BOT probe request is invalid"),
                    drogon::k400BadRequest,
                    dashboard_origin));
            } catch (const json::exception&) {
                callback(json_response(
                    problem(
                        "invalid_bot_probe_request",
                        "Interactive BOT probe request is invalid"),
                    drogon::k400BadRequest,
                    dashboard_origin));
            } catch (const std::exception&) {
                callback(json_response(
                    problem(
                        "bot_probe_failed",
                        "The verified adapter could not confirm a BOT account. "
                        "Review the visible readiness blockers and token-file access."),
                    drogon::k422UnprocessableEntity,
                    dashboard_origin));
            }
        },
        {drogon::Post, drogon::Options});
    application.registerHandler(
        "/v1/bot-control",
        [bearer_token, dashboard_origin, manager_data_root](
            const drogon::HttpRequestPtr& request,
            std::function<void(const drogon::HttpResponsePtr&)>&& callback) {
            if (request->method() == drogon::Options) {
                if (request->getHeader("Origin") != dashboard_origin) {
                    callback(json_response(
                        problem("origin_rejected", "Dashboard origin is not authorized"),
                        drogon::k403Forbidden,
                        dashboard_origin));
                    return;
                }
                callback(json_response(
                    json::object(), drogon::k204NoContent, dashboard_origin));
                return;
            }
            if (request->getHeader("Origin") != dashboard_origin ||
                request->getHeader("Authorization") !=
                    "Bearer " + bearer_token) {
                callback(json_response(
                    problem(
                        "picker_unauthorized",
                        "Interactive-user helper session is unavailable"),
                    drogon::k401Unauthorized,
                    dashboard_origin));
                return;
            }
            try {
                if (request->body().size() > 1024) {
                    throw std::invalid_argument("helper request is too large");
                }
                const json input = json::parse(request->body());
                if (!input.is_object() || input.size() != 2 ||
                    input.value("schema", "") !=
                        "arena_ui_bot_control_request_v1" ||
                    !input.contains("action") ||
                    !input.at("action").is_string()) {
                    throw std::invalid_argument(
                        "invalid BOT account-control request");
                }
                arena::application::ProcessSupervisor process;
                callback(json_response(
                    arena::application::ServiceCatalog::control_bot(
                        manager_data_root,
                        process,
                        input.at("action").get<std::string>()),
                    drogon::k200OK,
                    dashboard_origin));
            } catch (const std::invalid_argument&) {
                callback(json_response(
                    problem(
                        "invalid_bot_control_request",
                        "Interactive BOT account-control request is invalid"),
                    drogon::k400BadRequest,
                    dashboard_origin));
            } catch (const json::exception&) {
                callback(json_response(
                    problem(
                        "invalid_bot_control_request",
                        "Interactive BOT account-control request is invalid"),
                    drogon::k400BadRequest,
                    dashboard_origin));
            } catch (const std::exception&) {
                callback(json_response(
                    problem(
                        "bot_control_failed",
                        "The verified adapter could not complete BOT startup control. "
                        "Refresh status and review the visible readiness blockers."),
                    drogon::k422UnprocessableEntity,
                    dashboard_origin));
            }
        },
        {drogon::Post, drogon::Options});
    application.addListener("127.0.0.1", port);
    application.setThreadNum(1);
    application.disableSigtermHandling();
    application.run();
}

}  // namespace arena::cli
