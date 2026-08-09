#include "ui_picker.hpp"

#include "arena/application/process_supervisor.hpp"
#include "arena/application/service_catalog.hpp"
#include "arena/platform/operator_credential_file.hpp"

#include <drogon/drogon.h>
#include <nlohmann/json.hpp>
#include <sodium.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <functional>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#ifdef _WIN32
#include "arena/platform/windows_install_scope.hpp"
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

struct SetupRequest {
    std::string operation;
    std::string asset_kind;
    std::string component_id;
    std::filesystem::path path;
    std::vector<std::filesystem::path> roots;
    bool managed_copy{};
};

constexpr std::size_t max_setup_result_bytes = 1024 * 1024;
constexpr std::size_t max_lichess_token_bytes = 512;

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

SetupRequest parse_setup_request(const drogon::HttpRequestPtr& request) {
    if (request->body().size() > 64 * 1024) {
        throw std::invalid_argument("setup request is too large");
    }
    const json input = json::parse(request->body());
    if (!input.is_object() ||
        input.value("schema", "") != "arena_ui_setup_request_v1") {
        throw std::invalid_argument(
            "schema must be arena_ui_setup_request_v1");
    }
    SetupRequest result;
    result.operation = input.value("operation", "");
    if (result.operation == "discover") {
        if (input.size() < 2 || input.size() > 3 ||
            (input.size() == 3 && !input.contains("roots")) ||
            (input.contains("roots") &&
             (!input.at("roots").is_array() || input.at("roots").size() > 8))) {
            throw std::invalid_argument(
                "discover setup accepts at most eight absolute roots");
        }
        if (input.contains("roots")) {
            for (const auto& value : input.at("roots")) {
                if (!value.is_string()) {
                    throw std::invalid_argument(
                        "discover roots must be absolute path strings");
                }
                const std::string root = value.get<std::string>();
                const std::filesystem::path path(root);
                if (root.empty() || root.size() > 32767 ||
                    root.find('\0') != std::string::npos || !path.is_absolute()) {
                    throw std::invalid_argument(
                        "discover roots must be bounded absolute paths");
                }
                result.roots.push_back(path.lexically_normal());
            }
        }
        return result;
    }
    if (result.operation == "component") {
        static const std::set<std::string, std::less<>> allowed{
            "uci_arena.engine",
            "stockfish.engine",
            "node_runtime.private",
            "book_forge.service",
            "opening_book.standard",
            "uci_arena_evidence.runtime",
            "uci_arena_evidence.service",
            "lichess.adapter",
            "syzygy.wdl.3-4-5",
            "syzygy.dtz.3-4-5",
        };
        if (input.size() != 3 ||
            !input.contains("component_id") ||
            !input.at("component_id").is_string()) {
            throw std::invalid_argument(
                "component setup requires exactly one component id");
        }
        result.component_id =
            input.at("component_id").get<std::string>();
        if (!allowed.contains(result.component_id)) {
            throw std::invalid_argument(
                "component id is not available to desktop setup");
        }
        return result;
    }
    if (result.operation != "import") {
        throw std::invalid_argument(
            "setup operation must be discover, component, or import");
    }
    static const std::set<std::string, std::less<>> allowed{
        "opening_book",
        "syzygy",
        "uci_engine",
        "book_forge",
        "evidence_service",
        "timing_profile",
        "lichess_single_game_profile",
        "lichess_capacity_profile",
        "lichess_transport_profile",
        "lichess_token_path",
    };
    if (input.size() < 4 || input.size() > 5 ||
        !input.contains("asset_kind") ||
        !input.at("asset_kind").is_string() ||
        !input.contains("path") ||
        !input.at("path").is_string() ||
        (input.contains("managed_copy") &&
         !input.at("managed_copy").is_boolean())) {
        throw std::invalid_argument(
            "import setup requires one asset kind and one path");
    }
    static const std::set<std::string, std::less<>> allowed_fields{
        "schema", "operation", "asset_kind", "path", "managed_copy"};
    for (const auto& [field, value] : input.items()) {
        static_cast<void>(value);
        if (!allowed_fields.contains(field)) {
            throw std::invalid_argument(
                "import setup contains an unsupported field");
        }
    }
    result.asset_kind = input.at("asset_kind").get<std::string>();
    const std::string path = input.at("path").get<std::string>();
    result.managed_copy = input.value("managed_copy", false);
    if (!allowed.contains(result.asset_kind) ||
        path.empty() ||
        path.size() > 32767 ||
        (result.managed_copy &&
         result.asset_kind != "opening_book" &&
         result.asset_kind != "lichess_single_game_profile")) {
        throw std::invalid_argument(
            "import kind, path, or managed-copy mode is invalid");
    }
    result.path = std::filesystem::path(path);
    return result;
}

std::filesystem::path create_setup_result_path() {
    const auto result =
        std::filesystem::temp_directory_path() /
        ("uci-arena-setup-" + make_ui_picker_token() + ".json");
#ifdef _WIN32
    HANDLE file = CreateFileW(
        result.c_str(),
        GENERIC_READ | GENERIC_WRITE,
        0,
        nullptr,
        CREATE_NEW,
        FILE_ATTRIBUTE_TEMPORARY,
        nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        throw std::runtime_error(
            "cannot create the private setup result channel");
    }
    CloseHandle(file);
#else
    const int file = open(
        result.c_str(), O_CREAT | O_EXCL | O_RDWR | O_CLOEXEC, 0600);
    if (file < 0) {
        throw std::runtime_error(
            "cannot create the private setup result channel");
    }
    close(file);
#endif
    return result;
}

class ScopedHelperScratch final {
  public:
    ScopedHelperScratch()
        : path_(
              std::filesystem::temp_directory_path() /
              ("uci-arena-manager-ui-" +
#ifdef _WIN32
               std::to_string(GetCurrentProcessId())
#else
               std::to_string(getpid())
#endif
                   )) {
        std::filesystem::create_directories(path_);
    }

    ~ScopedHelperScratch() {
        std::error_code error;
        std::filesystem::remove_all(path_, error);
    }

    ScopedHelperScratch(const ScopedHelperScratch&) = delete;
    ScopedHelperScratch& operator=(const ScopedHelperScratch&) = delete;

    [[nodiscard]] const std::filesystem::path& path() const noexcept {
        return path_;
    }

  private:
    std::filesystem::path path_;
};

json read_setup_result(const std::filesystem::path& path) {
    std::error_code error;
    const auto size = std::filesystem::file_size(path, error);
    if (error || size == 0 || size > max_setup_result_bytes ||
        std::filesystem::is_symlink(path, error)) {
        throw std::runtime_error(
            "setup did not return a bounded machine-readable result");
    }
    std::ifstream input(path, std::ios::binary);
    json result;
    input >> result;
    if (!result.is_object()) {
        throw std::runtime_error("setup returned an invalid result");
    }
    return result;
}

#ifdef _WIN32
std::wstring widen(const std::string& value);

std::wstring quote_setup_argument(const std::wstring& value) {
    if (value.find_first_of(L" \t\n\v\"") == std::wstring::npos) {
        return value;
    }
    std::wstring result{L"\""};
    std::size_t slashes = 0;
    for (const wchar_t character : value) {
        if (character == L'\\') {
            ++slashes;
            continue;
        }
        if (character == L'"') {
            result.append(slashes * 2 + 1, L'\\');
            result.push_back(L'"');
            slashes = 0;
            continue;
        }
        result.append(slashes, L'\\');
        slashes = 0;
        result.push_back(character);
    }
    result.append(slashes * 2, L'\\');
    result.push_back(L'"');
    return result;
}

json run_setup_elevated(
    const std::filesystem::path& executable,
    const std::vector<std::string>& arguments,
    const std::filesystem::path& result_path) {
    std::wstring parameters;
    for (const auto& argument : arguments) {
        if (!parameters.empty()) {
            parameters.push_back(L' ');
        }
        parameters += quote_setup_argument(widen(argument));
    }
    SHELLEXECUTEINFOW execution{};
    execution.cbSize = sizeof(execution);
    execution.fMask = SEE_MASK_NOCLOSEPROCESS;
    execution.lpVerb = L"runas";
    execution.lpFile = executable.c_str();
    execution.lpParameters = parameters.c_str();
    const std::wstring directory = executable.parent_path().wstring();
    execution.lpDirectory = directory.c_str();
    execution.nShow = SW_HIDE;
    if (!ShellExecuteExW(&execution) || execution.hProcess == nullptr) {
        throw std::runtime_error(
            GetLastError() == ERROR_CANCELLED
                ? "Administrator confirmation was cancelled"
                : "The signed setup commit could not be elevated");
    }
    const DWORD waited =
        WaitForSingleObject(execution.hProcess, 10 * 60 * 1000);
    DWORD exit_code = 1;
    GetExitCodeProcess(execution.hProcess, &exit_code);
    CloseHandle(execution.hProcess);
    if (waited != WAIT_OBJECT_0) {
        throw std::runtime_error("The elevated setup commit timed out");
    }
    const json result = read_setup_result(result_path);
    if (result.value("schema", "") == "arena_setup_problem_v1") {
        throw std::runtime_error(
            result.value("detail", "The elevated setup commit failed"));
    }
    if (exit_code != 0) {
        throw std::runtime_error("The elevated setup commit did not succeed");
    }
    return result;
}
#endif

json run_setup(
    const std::filesystem::path& setup_executable,
    const std::filesystem::path& manager_data_root,
    const SetupRequest& request,
    const bool apply) {
    if (!std::filesystem::is_regular_file(setup_executable) ||
        std::filesystem::is_symlink(setup_executable)) {
        throw std::runtime_error(
            "The adjacent arena-setup executable is unavailable; repair UCI Arena Suite Setup");
    }
#ifdef _WIN32
    const bool per_user_install =
        arena::platform::is_windows_per_user_install();
#else
    const bool per_user_install = false;
#endif
    if (!apply && request.operation == "import") {
        return {
            {"schema", "arena_ui_setup_plan_v1"},
            {"operation", "import"},
            {"asset_kind", request.asset_kind},
            {"path", std::filesystem::absolute(request.path).lexically_normal().string()},
            {"mode",
             request.managed_copy
                 ? "bounded_managed_copy"
                 : "in_place_reference"},
            {"disk_impact_bytes",
             request.managed_copy ? json(nullptr) : json(0)},
            {"elevation_required", false},
            {"manager_service_restart", false},
            {"actions",
             json::array(
                 {{{"action", "validate_and_import"},
                   {"asset_kind", request.asset_kind}}})},
        };
    }
    if (apply && request.operation == "discover") {
        throw std::invalid_argument(
            "discover is a read-only setup-plan operation");
    }

    const std::filesystem::path result_path =
        create_setup_result_path();
    struct ResultFileGuard {
        std::filesystem::path path;
        ~ResultFileGuard() {
            std::error_code ignored;
            std::filesystem::remove(path, ignored);
        }
    } guard{result_path};

    std::vector<std::string> arguments{
        "--data-root",
        manager_data_root.string(),
#ifdef _WIN32
        "--install-scope",
        per_user_install ? "per-user" : "per-machine",
#endif
        "--json",
        "--result-file",
        result_path.string(),
    };
    if (request.operation == "discover") {
        arguments.push_back("inventory");
        for (const auto& root : request.roots) {
            arguments.push_back("--scan-root");
            arguments.push_back(root.string());
        }
    } else if (request.operation == "component") {
        if (apply && !per_user_install) {
            arguments.push_back("--restart-manager-service");
        }
        arguments.insert(
            arguments.end(),
            {
                apply ? "apply" : "plan",
                "--preset",
                "custom",
                "--component",
                request.component_id,
            });
        if (apply) {
            arguments.push_back("--yes");
        }
    } else {
        arguments.insert(
            arguments.end(),
            {
                "import",
                "--kind",
                request.asset_kind,
                "--path",
                request.path.string(),
            });
        if (request.managed_copy) {
            arguments.push_back("--managed-copy");
        }
    }

    if (apply && request.operation == "component") {
#ifdef _WIN32
        if (!per_user_install) {
            return run_setup_elevated(
                setup_executable, arguments, result_path);
        }
#else
        arena::application::ProcessSupervisor process;
        if (geteuid() != 0 &&
            std::filesystem::is_regular_file("/usr/bin/pkexec")) {
            std::vector<std::string> elevated{
                setup_executable.string()};
            elevated.insert(
                elevated.end(), arguments.begin(), arguments.end());
            try {
                return process.run_json(
                    "/usr/bin/pkexec",
                    setup_executable.parent_path(),
                    elevated,
                    std::chrono::minutes(10));
            } catch (const std::exception&) {
                const json result = read_setup_result(result_path);
                if (result.value("schema", "") ==
                    "arena_setup_problem_v1") {
                    throw std::runtime_error(
                        result.value(
                            "detail",
                            "The elevated setup commit failed"));
                }
                throw;
            }
        }
#endif
    }

    arena::application::ProcessSupervisor process;
    try {
        json result = process.run_json(
            setup_executable,
            setup_executable.parent_path(),
            arguments,
            apply ? std::chrono::minutes(10) :
                    std::chrono::minutes(2));
        if (!apply && request.operation == "component") {
            result["operation"] = "component";
            result["elevation_required"] = !per_user_install;
            result["manager_service_restart"] = !per_user_install;
        } else if (request.operation == "discover") {
            result["operation"] = "discover";
        }
        return result;
    } catch (const std::exception&) {
        const json result = read_setup_result(result_path);
        if (result.value("schema", "") == "arena_setup_problem_v1") {
            throw std::runtime_error(
                result.value("detail", "Local setup failed"));
        }
        throw;
    }
}

json plan_lichess_credential() {
    return {
        {"schema", "arena_ui_credential_plan_v1"},
        {"credential", "lichess_bot_token"},
        {"storage", "private_operator_external_file"},
        {"elevation_required", false},
        {"manager_service_restart", false},
        {"provider_startup_must_be_disabled", true},
        {"actions",
         json::array({
             {{"action", "materialize_private_operator_file"}},
             {{"action", "commit_installer_locator"}},
             {{"action", "regenerate_consumer_configuration"}},
         })},
    };
}

bool valid_direct_lichess_token(const std::string_view value) {
    return !value.empty() && value.size() <= max_lichess_token_bytes &&
           std::none_of(
               value.begin(), value.end(), [](const unsigned char character) {
                   return character == 0 || character == '\r' ||
                          character == '\n' || std::isspace(character) != 0;
               });
}

json materialize_lichess_credential(
    const std::string_view credential,
    const std::filesystem::path& manager_data_root,
    const std::filesystem::path& setup_executable) {
    if (!valid_direct_lichess_token(credential)) {
        throw std::invalid_argument(
            "the API key must be one bounded value without whitespace");
    }

    arena::application::ProcessSupervisor process;
    const json status = arena::application::ServiceCatalog::control_bot(
        manager_data_root, process, "status");
    if (status.value("installed", false)) {
        throw std::runtime_error(
            "Disable Lichess Bot startup before replacing its API key");
    }

    const std::filesystem::path destination =
        arena::platform::default_operator_credential_root() /
        "lichess-bot.token";
    arena::platform::OperatorCredentialFileUpdate update(
        destination, credential);
    SetupRequest import;
    import.operation = "import";
    import.asset_kind = "lichess_token_path";
    import.path = update.path();
    static_cast<void>(run_setup(
        setup_executable, manager_data_root, import, true));
    update.commit();
    return {
        {"schema", "arena_ui_credential_result_v1"},
        {"state", "committed"},
        {"credential", "lichess_bot_token"},
        {"locator_committed", true},
        {"consumer_configuration_regenerated", true},
    };
}

json qualify_and_import_lichess_profiles(
    const std::filesystem::path& manager_data_root,
    const std::filesystem::path& setup_executable) {
    arena::application::ProcessSupervisor process;
    const json qualification =
        arena::application::ServiceCatalog::qualify_bot(
            manager_data_root, process);

    SetupRequest single_game;
    single_game.operation = "import";
    single_game.asset_kind = "lichess_single_game_profile";
    single_game.path = qualification.at("single_game_profile").get<std::string>();
    single_game.managed_copy = true;
    static_cast<void>(run_setup(
        setup_executable, manager_data_root, single_game, true));

    SetupRequest capacity;
    capacity.operation = "import";
    capacity.asset_kind = "lichess_capacity_profile";
    capacity.path = qualification.at("capacity_profile").get<std::string>();
    static_cast<void>(run_setup(
        setup_executable, manager_data_root, capacity, true));

    return {
        {"schema", "arena_bot_qualification_result_v1"},
        {"schema_version", 1},
        {"state", "committed"},
        {"single_game_profile_imported", true},
        {"capacity_profile_imported", true},
        {"reused", qualification.at("reused")},
        {"maximum_concurrency", qualification.at("maximum_concurrency")},
        {"search_core", qualification.at("search_core")},
        {"model_id", qualification.at("model_id")},
        {"provider", qualification.at("provider")},
        {"timing_identity_sha256",
         qualification.at("timing_identity_sha256")},
    };
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
    const std::filesystem::path& manager_data_root,
    const std::filesystem::path& setup_executable,
    const bool allow_setup) {
    ScopedHelperScratch scratch;
    auto& application = drogon::app();
    application.setUploadPath(scratch.path().string());
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
    application.registerHandler(
        "/v1/bot-options",
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
                if (request->body().size() > 64 * 1024) {
                    throw std::invalid_argument("BOT options request is too large");
                }
                const json input = json::parse(request->body());
                if (!input.is_object() ||
                    input.value("schema", "") !=
                        "arena_ui_bot_options_request_v1" ||
                    !input.contains("operation") ||
                    !input.at("operation").is_string()) {
                    throw std::invalid_argument("invalid BOT options request");
                }
                const std::string operation =
                    input.at("operation").get<std::string>();
                arena::application::ProcessSupervisor process;
                if (operation == "get" && input.size() == 2) {
                    callback(json_response(
                        arena::application::ServiceCatalog::bot_options(
                            manager_data_root, process),
                        drogon::k200OK,
                        dashboard_origin));
                    return;
                }
                if (operation != "update" || input.size() != 5 ||
                    !input.contains("settings") ||
                    !input.at("settings").is_object() ||
                    !input.contains("confirm_rated") ||
                    !input.at("confirm_rated").is_boolean() ||
                    !input.contains("confirm_outbound") ||
                    !input.at("confirm_outbound").is_boolean()) {
                    throw std::invalid_argument("invalid BOT options request");
                }
                callback(json_response(
                    arena::application::ServiceCatalog::update_bot_options(
                        manager_data_root,
                        process,
                        input.at("settings"),
                        input.at("confirm_rated").get<bool>(),
                        input.at("confirm_outbound").get<bool>()),
                    drogon::k200OK,
                    dashboard_origin));
            } catch (const std::invalid_argument&) {
                callback(json_response(
                    problem(
                        "invalid_bot_options_request",
                        "Interactive BOT options request is invalid"),
                    drogon::k400BadRequest,
                    dashboard_origin));
            } catch (const json::exception&) {
                callback(json_response(
                    problem(
                        "invalid_bot_options_request",
                        "Interactive BOT options request is invalid"),
                    drogon::k400BadRequest,
                    dashboard_origin));
            } catch (const std::exception& error) {
                callback(json_response(
                    problem(
                        "bot_options_failed",
                        std::string("The verified adapter rejected the BOT options: ") +
                            error.what()),
                    drogon::k422UnprocessableEntity,
                    dashboard_origin));
            }
        },
        {drogon::Post, drogon::Options});
    application.registerHandler(
        "/v1/bot-account",
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
                if (request->body().size() > 4096) {
                    throw std::invalid_argument(
                        "BOT account-control request is too large");
                }
                const json input = json::parse(request->body());
                if (!input.is_object() || input.size() != 3 ||
                    input.value("schema", "") !=
                        "arena_ui_bot_account_request_v1" ||
                    !input.contains("action") ||
                    !input.at("action").is_string() ||
                    !input.contains("parameters") ||
                    !input.at("parameters").is_object()) {
                    throw std::invalid_argument(
                        "invalid BOT account-control request");
                }
                arena::application::ProcessSupervisor process;
                callback(json_response(
                    arena::application::ServiceCatalog::control_bot_account(
                        manager_data_root,
                        process,
                        input.at("action").get<std::string>(),
                        input.at("parameters")),
                    drogon::k200OK,
                    dashboard_origin));
            } catch (const std::invalid_argument&) {
                callback(json_response(
                    problem(
                        "invalid_bot_account_request",
                        "Interactive BOT account-control request is invalid"),
                    drogon::k400BadRequest,
                    dashboard_origin));
            } catch (const json::exception&) {
                callback(json_response(
                    problem(
                        "invalid_bot_account_request",
                        "Interactive BOT account-control request is invalid"),
                    drogon::k400BadRequest,
                    dashboard_origin));
            } catch (const std::exception& error) {
                callback(json_response(
                    problem(
                        "bot_account_failed",
                        std::string("The verified adapter rejected BOT account control: ") +
                            error.what()),
                    drogon::k422UnprocessableEntity,
                    dashboard_origin));
            }
        },
        {drogon::Post, drogon::Options});
    if (allow_setup) {
        application.registerHandler(
            "/v1/bot-qualification",
            [bearer_token,
             dashboard_origin,
             manager_data_root,
             setup_executable](
                const drogon::HttpRequestPtr& request,
                std::function<void(const drogon::HttpResponsePtr&)>&& callback) {
                if (request->method() == drogon::Options) {
                    if (request->getHeader("Origin") != dashboard_origin) {
                        callback(json_response(
                            problem(
                                "origin_rejected",
                                "Dashboard origin is not authorized"),
                            drogon::k403Forbidden,
                            dashboard_origin));
                        return;
                    }
                    callback(json_response(
                        json::object(),
                        drogon::k204NoContent,
                        dashboard_origin));
                    return;
                }
                if (request->getHeader("Origin") != dashboard_origin ||
                    request->getHeader("Authorization") !=
                        "Bearer " + bearer_token) {
                    callback(json_response(
                        problem(
                            "qualification_unauthorized",
                            "Native installed qualification capability is unavailable"),
                        drogon::k401Unauthorized,
                        dashboard_origin));
                    return;
                }
                try {
                    if (request->body().size() > 1024) {
                        throw std::invalid_argument(
                            "installed qualification request is too large");
                    }
                    const json input = json::parse(request->body());
                    if (!input.is_object() || input.size() != 1 ||
                        input.value("schema", "") !=
                            "arena_ui_bot_qualification_request_v1") {
                        throw std::invalid_argument(
                            "invalid installed qualification request");
                    }
                    callback(json_response(
                        qualify_and_import_lichess_profiles(
                            manager_data_root, setup_executable),
                        drogon::k200OK,
                        dashboard_origin));
                } catch (const std::invalid_argument&) {
                    callback(json_response(
                        problem(
                            "invalid_bot_qualification_request",
                            "Installed BOT qualification request is invalid"),
                        drogon::k400BadRequest,
                        dashboard_origin));
                } catch (const json::exception&) {
                    callback(json_response(
                        problem(
                            "invalid_bot_qualification_request",
                            "Installed BOT qualification request is invalid"),
                        drogon::k400BadRequest,
                        dashboard_origin));
                } catch (
                    const arena::application::BotQualificationFailure& error) {
                    callback(json_response(
                        problem(
                            error.code(),
                            "Installed Engine qualification did not commit during " +
                                error.stage() + ": " + error.detail() +
                                ". No profile locators changed. Correct the reported provider condition and retry."),
                        drogon::k422UnprocessableEntity,
                        dashboard_origin));
                } catch (const std::exception&) {
                    callback(json_response(
                        problem(
                            "bot_qualification_failed",
                            "Installed Engine qualification did not commit. Disable BOT startup, then review Engine, Evidence, timing, and knowledge readiness before retrying."),
                        drogon::k422UnprocessableEntity,
                        dashboard_origin));
                }
            },
            {drogon::Post, drogon::Options});
        application.registerHandler(
            "/v1/bot-credential-plan",
            [bearer_token, dashboard_origin](
                const drogon::HttpRequestPtr& request,
                std::function<void(const drogon::HttpResponsePtr&)>&& callback) {
                if (request->method() == drogon::Options) {
                    if (request->getHeader("Origin") != dashboard_origin) {
                        callback(json_response(
                            problem(
                                "origin_rejected",
                                "Dashboard origin is not authorized"),
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
                            "credential_unauthorized",
                            "Native credential capability is unavailable"),
                        drogon::k401Unauthorized,
                        dashboard_origin));
                    return;
                }
                try {
                    if (request->body().size() > 1024) {
                        throw std::invalid_argument(
                            "credential plan request is too large");
                    }
                    const json input = json::parse(request->body());
                    if (!input.is_object() || input.size() != 1 ||
                        input.value("schema", "") !=
                            "arena_ui_credential_plan_request_v1") {
                        throw std::invalid_argument(
                            "invalid credential plan request");
                    }
                    callback(json_response(
                        plan_lichess_credential(),
                        drogon::k200OK,
                        dashboard_origin));
                } catch (const std::exception&) {
                    callback(json_response(
                        problem(
                            "invalid_credential_plan_request",
                            "Native credential plan request is invalid"),
                        drogon::k400BadRequest,
                        dashboard_origin));
                }
            },
            {drogon::Post, drogon::Options});
        application.registerHandler(
            "/v1/bot-credential-apply",
            [bearer_token,
             dashboard_origin,
             manager_data_root,
             setup_executable](
                const drogon::HttpRequestPtr& request,
                std::function<void(const drogon::HttpResponsePtr&)>&& callback) {
                if (request->method() == drogon::Options) {
                    if (request->getHeader("Origin") != dashboard_origin) {
                        callback(json_response(
                            problem(
                                "origin_rejected",
                                "Dashboard origin is not authorized"),
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
                            "credential_unauthorized",
                            "Native credential capability is unavailable"),
                        drogon::k401Unauthorized,
                        dashboard_origin));
                    return;
                }
                try {
                    if (request->getHeader("Content-Type") !=
                            "application/octet-stream" ||
                        request->body().empty() ||
                        request->body().size() > max_lichess_token_bytes) {
                        throw std::invalid_argument(
                            "credential materialization request is invalid");
                    }
                    callback(json_response(
                        materialize_lichess_credential(
                            request->body(),
                            manager_data_root,
                            setup_executable),
                        drogon::k200OK,
                        dashboard_origin));
                } catch (const std::invalid_argument&) {
                    callback(json_response(
                        problem(
                            "invalid_credential_request",
                            "Enter one API key without spaces or line breaks"),
                        drogon::k400BadRequest,
                        dashboard_origin));
                } catch (const std::exception&) {
                    callback(json_response(
                        problem(
                            "credential_setup_failed",
                            "The API key could not be saved; the preceding credential remains authoritative"),
                        drogon::k422UnprocessableEntity,
                        dashboard_origin));
                }
            },
            {drogon::Post, drogon::Options});
        const auto register_setup_handler =
            [&application,
             bearer_token,
             dashboard_origin,
             manager_data_root,
             setup_executable](
                const std::string& path,
                const bool apply) {
                application.registerHandler(
                    path,
                    [bearer_token,
                     dashboard_origin,
                     manager_data_root,
                     setup_executable,
                     apply](
                        const drogon::HttpRequestPtr& request,
                        std::function<void(
                            const drogon::HttpResponsePtr&)>&&
                            callback) {
                        if (request->method() == drogon::Options) {
                            if (request->getHeader("Origin") !=
                                dashboard_origin) {
                                callback(json_response(
                                    problem(
                                        "origin_rejected",
                                        "Dashboard origin is not authorized"),
                                    drogon::k403Forbidden,
                                    dashboard_origin));
                                return;
                            }
                            callback(json_response(
                                json::object(),
                                drogon::k204NoContent,
                                dashboard_origin));
                            return;
                        }
                        if (request->getHeader("Origin") !=
                                dashboard_origin ||
                            request->getHeader("Authorization") !=
                                "Bearer " + bearer_token) {
                            callback(json_response(
                                problem(
                                    "setup_unauthorized",
                                    "Native setup capability is unavailable"),
                                drogon::k401Unauthorized,
                                dashboard_origin));
                            return;
                        }
                        try {
                            callback(json_response(
                                run_setup(
                                    setup_executable,
                                    manager_data_root,
                                    parse_setup_request(request),
                                    apply),
                                drogon::k200OK,
                                dashboard_origin));
                        } catch (const std::invalid_argument& error) {
                            callback(json_response(
                                problem(
                                    "invalid_setup_request",
                                    error.what()),
                                drogon::k400BadRequest,
                                dashboard_origin));
                        } catch (const json::exception&) {
                            callback(json_response(
                                problem(
                                    "invalid_setup_request",
                                    "Native setup request is invalid"),
                                drogon::k400BadRequest,
                                dashboard_origin));
                        } catch (const std::exception& error) {
                            callback(json_response(
                                problem("setup_failed", error.what()),
                                drogon::k422UnprocessableEntity,
                                dashboard_origin));
                        }
                    },
                    {drogon::Post, drogon::Options});
            };
        register_setup_handler("/v1/setup-plan", false);
        register_setup_handler("/v1/setup-apply", true);
    }
    application.addListener("127.0.0.1", port);
    application.setThreadNum(1);
    application.run();
}

}  // namespace arena::cli
