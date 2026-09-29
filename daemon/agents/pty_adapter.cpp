#include "daemon/agents/pty_adapter.h"

#include <cerrno>
#include <csignal>
#include <cstring>
#include <pty.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <unistd.h>

#include <chrono>

namespace aegis::daemon::agents {
namespace {

std::int64_t now() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

std::string eventId() {
    static std::atomic_uint64_t sequence = 0;
    return "event-" + std::to_string(now()) + "-" + std::to_string(++sequence);
}

}

PtyAdapter::PtyAdapter(std::string name, std::vector<std::string> command, EventSink sink)
    : name_(std::move(name)), command_(std::move(command)), sink_(std::move(sink)) {}

PtyAdapter::~PtyAdapter() {
    terminate();
}

bool PtyAdapter::start(const RunContext &context) {
    terminate();
    context_ = context;
    if (command_.empty()) return false;
    std::vector<char *> argv;
    argv.reserve(command_.size() + 1);
    for (auto &argument : command_) argv.push_back(argument.data());
    argv.push_back(nullptr);
    winsize size{};
    size.ws_col = 100;
    size.ws_row = 30;
    int master = -1;
    const auto child = forkpty(&master, nullptr, nullptr, &size);
    if (child < 0) return false;
    if (child == 0) {
        if (chdir(context_.repository.c_str()) != 0) _exit(126);
        setenv("TERM", "xterm-256color", 1);
        execvp(argv.front(), argv.data());
        _exit(127);
    }
    {
        std::lock_guard lock(writeMutex_);
        pid_ = child;
        master_ = master;
    }
    running_ = true;
    reader_ = std::thread([this, master, child] { readLoop(master, child); });
    return true;
}

SendResult PtyAdapter::send(std::string_view message) {
    std::string line(message);
    line += '\n';
    return sendPty(line) ? SendResult::accepted : SendResult::unavailable;
}

bool PtyAdapter::sendPty(std::string_view input) {
    std::lock_guard lock(writeMutex_);
    if (!running_ || master_ < 0) return false;
    const char *cursor = input.data();
    auto remaining = input.size();
    while (remaining > 0) {
        const auto written = ::write(master_, cursor, remaining);
        if (written < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        cursor += written;
        remaining -= static_cast<std::size_t>(written);
    }
    return true;
}

bool PtyAdapter::resizePty(unsigned short cols, unsigned short rows) {
    std::lock_guard lock(writeMutex_);
    if (!running_ || master_ < 0 || cols == 0 || rows == 0) return false;
    winsize size{};
    size.ws_col = cols;
    size.ws_row = rows;
    if (ioctl(master_, TIOCSWINSZ, &size) != 0) return false;
    if (pid_ > 0) kill(pid_, SIGWINCH);
    return true;
}

void PtyAdapter::interrupt() {
    int child;
    {
        std::lock_guard lock(writeMutex_);
        child = pid_;
    }
    if (child > 0) {
        kill(-child, SIGINT);
        publish("turn.interrupted", "");
    }
}

void PtyAdapter::terminate() {
    int child;
    running_ = false;
    {
        std::lock_guard lock(writeMutex_);
        child = pid_;
    }
    if (child > 0) {
        const auto processGroup = child;
        kill(-child, SIGTERM);
        kill(child, SIGTERM);
        usleep(300000);
        kill(-processGroup, SIGKILL);
    }
    if (reader_.joinable()) reader_.join();
    std::lock_guard lock(writeMutex_);
    if (master_ >= 0) close(master_);
    master_ = -1;
}

void PtyAdapter::readLoop(int master, int child) {
    char buffer[8192];
    while (running_) {
        const auto count = read(master, buffer, sizeof(buffer));
        if (count > 0) {
            publish("agent.message.delta", std::string(buffer, static_cast<std::size_t>(count)));
            continue;
        }
        if (count < 0 && errno == EINTR) continue;
        break;
    }
    int status = 0;
    while (waitpid(child, &status, 0) < 0 && errno == EINTR) {}
    {
        std::lock_guard lock(writeMutex_);
        if (pid_ == child) pid_ = -1;
        if (master_ == master) {
            close(master_);
            master_ = -1;
        }
    }
    if (running_.exchange(false)) {
        if (WIFEXITED(status) && WEXITSTATUS(status) == 0) publish("run.completed", "");
        else if (WIFSIGNALED(status) && WTERMSIG(status) == SIGINT) publish("run.interrupted", "");
        else publish("run.failed", WIFEXITED(status)
            ? "Agent exited with status " + std::to_string(WEXITSTATUS(status))
            : "Agent process terminated");
    }
}

void PtyAdapter::publish(std::string type, std::string content) {
    emitEvent(sink_, {eventId(), context_.taskId, context_.runId, std::move(type), name_, std::move(content), now()});
}

}
