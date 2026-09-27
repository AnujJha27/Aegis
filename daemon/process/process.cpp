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

ChildProcess::~ChildProcess() {
    terminate();
    if (pid_ > 0) (void)wait(std::chrono::seconds(2));
    else if (output_ >= 0) close(output_);
}

bool ChildProcess::start(const std::vector<std::string> &arguments, const std::filesystem::path &directory) {
    if (arguments.empty()) return false;
    int outputPipe[2];
    if (pipe(outputPipe) != 0) return false;
    const auto child = fork();
    if (child < 0) {
        close(outputPipe[0]); close(outputPipe[1]);
        return false;
    }
    if (child == 0) {
        setpgid(0, 0);
        if (chdir(directory.c_str()) != 0) _exit(126);
        dup2(outputPipe[1], STDOUT_FILENO);
        dup2(outputPipe[1], STDERR_FILENO);
        close(outputPipe[0]); close(outputPipe[1]);
        std::vector<char *> argv;
        argv.reserve(arguments.size() + 1);
        for (const auto &argument : arguments) argv.push_back(const_cast<char *>(argument.c_str()));
        argv.push_back(nullptr);
        execvp(argv.front(), argv.data());
        _exit(127);
    }

    close(outputPipe[1]);
    const auto flags = fcntl(outputPipe[0], F_GETFL, 0);
    if (flags >= 0) fcntl(outputPipe[0], F_SETFL, flags | O_NONBLOCK);
    std::lock_guard lock(mutex_);
    pid_ = child;
    output_ = outputPipe[0];
    interruptRequested_ = false;
    terminationRequested_ = false;
    return true;
}

Result ChildProcess::wait(std::chrono::seconds timeout,
                          const std::function<void(std::string_view)> &onOutput,
                          bool captureOutput) {
    pid_t child;
    int output;
    {
        std::lock_guard lock(mutex_);
        child = pid_;
        output = output_;
    }
    if (child <= 0) return {-1, "process is not running", false};

    Result result;
    int status = 0;
    bool finished = false;
    bool outputClosed = output < 0;
    bool timedOut = false;
    bool sentTerm = false;
    bool sawInterrupt = false;
    std::chrono::steady_clock::time_point termAt{};
    std::chrono::steady_clock::time_point interruptAt{};
    std::chrono::steady_clock::time_point finishedAt{};
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    char buffer[8192];
    while (!finished || !outputClosed) {
        if (!outputClosed) {
            pollfd descriptor{output, POLLIN | POLLHUP, 0};
            (void)poll(&descriptor, 1, 50);
            for (;;) {
                const auto count = read(output, buffer, sizeof(buffer));
                if (count > 0) {
                    const std::string_view chunk(buffer, static_cast<std::size_t>(count));
                    if (captureOutput) result.output.append(chunk);
                    if (onOutput) onOutput(chunk);
                    continue;
                }
                if (count == 0 || (count < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)) outputClosed = true;
                break;
            }
        }

        if (!finished) {
            std::lock_guard lock(mutex_);
            const auto waited = waitpid(pid_, &status, WNOHANG);
            if (waited == child || (waited < 0 && errno == ECHILD)) {
                pid_ = -1;
                finished = true;
                finishedAt = std::chrono::steady_clock::now();
            }
            if (!sawInterrupt && interruptRequested_) {
                sawInterrupt = true;
                interruptAt = std::chrono::steady_clock::now();
            }
            if (!sentTerm && (terminationRequested_ || std::chrono::steady_clock::now() >= deadline)) {
                timedOut = !terminationRequested_;
                if (pid_ > 0) { kill(-pid_, SIGTERM); kill(pid_, SIGTERM); }
                sentTerm = true;
                termAt = std::chrono::steady_clock::now();
            }
            if (!sentTerm && sawInterrupt && std::chrono::steady_clock::now() - interruptAt > std::chrono::milliseconds(300)) {
                if (pid_ > 0) { kill(-pid_, SIGTERM); kill(pid_, SIGTERM); }
                sentTerm = true;
                termAt = std::chrono::steady_clock::now();
            }
            if (sentTerm && !finished && std::chrono::steady_clock::now() - termAt > std::chrono::milliseconds(300) && pid_ > 0) {
                kill(-pid_, SIGKILL); kill(pid_, SIGKILL);
            }
        }

        if (finished && !outputClosed && std::chrono::steady_clock::now() - finishedAt > std::chrono::milliseconds(500)) {
            kill(-child, SIGTERM);
            outputClosed = true;
        }
    }

    {
        std::lock_guard lock(mutex_);
        if (output_ >= 0) close(output_);
        output_ = -1;
        interruptRequested_ = false;
        terminationRequested_ = false;
    }
    result.timedOut = timedOut;
    if (WIFEXITED(status)) result.exitCode = WEXITSTATUS(status);
    else if (WIFSIGNALED(status)) result.exitCode = 128 + WTERMSIG(status);
    return result;
}

void ChildProcess::interrupt() {
    std::lock_guard lock(mutex_);
    if (pid_ > 0) {
        interruptRequested_ = true;
        kill(-pid_, SIGINT);
        kill(pid_, SIGINT);
    }
}

void ChildProcess::terminate() {
    std::lock_guard lock(mutex_);
    if (pid_ > 0) {
        terminationRequested_ = true;
        kill(-pid_, SIGTERM);
        kill(pid_, SIGTERM);
    }
}

Result run(const std::vector<std::string> &arguments,
           const std::filesystem::path &directory,
           std::chrono::seconds timeout) {
    ChildProcess child;
    if (!child.start(arguments, directory)) return {-1, std::strerror(errno), false};
    return child.wait(timeout);
}

}
