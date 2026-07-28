#include "arena/application/broker_policy.hpp"

#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <stdexcept>

namespace arena::application {

int perform_service_action(const std::string& platform_name, const std::string& action) {
    const pid_t child = fork();
    if (child < 0) {
        throw std::runtime_error("cannot fork systemctl");
    }
    if (child == 0) {
        execl(
            "/usr/bin/systemctl",
            "systemctl",
            action.c_str(),
            "--",
            platform_name.c_str(),
            static_cast<char*>(nullptr));
        _exit(127);
    }
    int status = 0;
    if (waitpid(child, &status, 0) != child || !WIFEXITED(status)) {
        throw std::runtime_error("systemctl did not exit normally");
    }
    return WEXITSTATUS(status);
}

}  // namespace arena::application
