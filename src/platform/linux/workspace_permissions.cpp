#include "arena/platform/workspace_permissions.hpp"

#include <spawn.h>
#include <sys/wait.h>

#include <array>
#include <cerrno>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <vector>

extern char** environ;

namespace arena::platform {
namespace {

std::filesystem::path setfacl_executable() {
    for (const auto& candidate :
         std::array{std::filesystem::path{"/usr/bin/setfacl"},
                    std::filesystem::path{"/bin/setfacl"}}) {
        if (std::filesystem::is_regular_file(candidate)) {
            return candidate;
        }
    }
    throw std::runtime_error(
        "managed workspace permission setup requires the acl package");
}

void run_setfacl(const std::filesystem::path& executable,
                 const std::vector<std::string>& arguments) {
    std::vector<std::string> owned;
    owned.reserve(arguments.size() + 1);
    owned.push_back(executable.string());
    owned.insert(owned.end(), arguments.begin(), arguments.end());
    std::vector<char*> argv;
    argv.reserve(owned.size() + 1);
    for (auto& value : owned) {
        argv.push_back(value.data());
    }
    argv.push_back(nullptr);

    pid_t child = 0;
    const int spawn_result =
        posix_spawn(&child, executable.c_str(), nullptr, nullptr, argv.data(), environ);
    if (spawn_result != 0) {
        throw std::runtime_error("cannot start setfacl: error " +
                                 std::to_string(spawn_result));
    }
    int status = 0;
    while (waitpid(child, &status, 0) == -1) {
        if (errno != EINTR) {
            throw std::runtime_error("cannot wait for setfacl");
        }
    }
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        throw std::runtime_error("setfacl rejected the managed service workspace");
    }
}

} // namespace

void grant_manager_workspace_access(
    const std::filesystem::path& workspace,
    const bool manager_service_access) {
    if (!std::filesystem::is_directory(workspace) ||
        std::filesystem::is_symlink(workspace)) {
        throw std::runtime_error(
            "managed service workspace is unavailable for permission setup");
    }
    if (!manager_service_access) {
        return;
    }
    const auto executable = setfacl_executable();
    const std::string path = workspace.string();
    run_setfacl(
        executable,
        {"--physical", "--recursive", "--modify", "u:uci-arena:rwX", path});
    run_setfacl(
        executable,
        {"--physical", "--modify",
         "u:uci-arena:rwx,d:u:uci-arena:rwx", path});
}

} // namespace arena::platform
