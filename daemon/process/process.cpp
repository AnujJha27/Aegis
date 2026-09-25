#include "daemon/process/process.h"

#include <cerrno>
#include <csignal>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <sys/wait.h>
#include <unistd.h>

#include <chrono>

namespace aegis::daemon::process {

Result run(const std::vector<std::string> &arguments, const std::filesystem::path &directory, std::chrono::seconds timeout) {
    if (arguments.empty()) return {-1, "empty command", false};

    int outputPipe[2];
    if (pipe(outputPipe) != 0) return {-1, std::strerror(errno), false};
    const auto pid = fork();
    if (pid < 0) {
        close(outputPipe[0]);
        close(outputPipe[1]);
        return {-1, std::strerror(errno), false};
    }
    if (pid == 0) {
        setpgid(0, 0);
        if (chdir(directory.c_str()) != 0) _exit(126);
        dup2(outputPipe[1], STDOUT_FILENO);
        dup2(outputPipe[1], STDERR_FILENO);
        close(outputPipe[0]);
        close(outputPipe[1]);
        std::vector<char *> argv;
        argv.reserve(arguments.size() + 1);
        for (const auto &argument : arguments) argv.push_back(const_cast<char *>(argument.c_str()));
        argv.push_back(nullptr);
        execvp(argv.front(), argv.data());
        _exit(127);
    }

    close(outputPipe[1]);
    fcntl(outputPipe[0], F_SETFL, fcntl(outputPipe[0], F_GETFL) | O_NONBLOCK);
    Result result;
    int status = 0;
    bool finished = false;
    bool outputClosed = false;
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    char buffer[8192];
    while (!finished || !outputClosed) {
        pollfd descriptor{outputPipe[0], POLLIN, 0};
        poll(&descriptor, 1, 50);
        for (;;) {
            const auto count = read(outputPipe[0], buffer, sizeof(buffer));
            if (count > 0) {
                result.output.append(buffer, static_cast<std::size_t>(count));
                continue;
            }
            if (count == 0) outputClosed = true;
            break;
        }
        if (!finished) {
            const auto waited = waitpid(pid, &status, WNOHANG);
            if (waited == pid || (waited < 0 && errno == ECHILD)) finished = true;
        }
        if (!finished && std::chrono::steady_clock::now() >= deadline) {
            result.timedOut = true;
            kill(-pid, SIGTERM);
            kill(pid, SIGTERM);
            waitpid(pid, &status, 0);
            finished = true;
        }
    }
    close(outputPipe[0]);
    if (WIFEXITED(status)) result.exitCode = WEXITSTATUS(status);
    else if (WIFSIGNALED(status)) result.exitCode = 128 + WTERMSIG(status);
    return result;
}

}
