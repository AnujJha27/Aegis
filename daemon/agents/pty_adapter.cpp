#include "daemon/agents/pty_adapter.h"

#include <cerrno>
#include <csignal>
#include <cstring>
#include <pty.h>
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
    pid_ = forkpty(&master_, nullptr, nullptr, nullptr);
    if (pid_ < 0) return false;
    if (pid_ == 0) {
        if (chdir(context_.repository.c_str()) != 0) _exit(126);
        setenv("TERM", "xterm-256color", 1);
        execvp(argv.front(), argv.data());
        _exit(127);
    }
    running_ = true;
    publish("agent.started", "");
    reader_ = std::thread([this] { readLoop(); });
    return true;
}

void PtyAdapter::send(std::string_view message) {
    std::lock_guard lock(writeMutex_);
    if (!running_ || master_ < 0) return;
    std::string line(message);
    line += '\n';
    const char *cursor = line.data();
    auto remaining = line.size();
    while (remaining > 0) {
        const auto written = ::write(master_, cursor, remaining);
        if (written < 0) {
            if (errno == EINTR) continue;
            return;
        }
        cursor += written;
        remaining -= static_cast<std::size_t>(written);
    }
    publish("agent.message.sent", std::string(message));
}

void PtyAdapter::interrupt() {
    if (pid_ > 0) kill(-pid_, SIGINT);
}

void PtyAdapter::terminate() {
    if (pid_ <= 0 && !reader_.joinable()) return;
    running_ = false;
    if (pid_ > 0) {
        kill(-pid_, SIGTERM);
        kill(pid_, SIGTERM);
        int status = 0;
        for (int attempt = 0; attempt < 100; ++attempt) {
            const auto result = waitpid(pid_, &status, WNOHANG);
            if (result == pid_ || (result < 0 && errno == ECHILD)) break;
            usleep(10000);
        }
        if (kill(pid_, 0) == 0) {
            kill(-pid_, SIGKILL);
            kill(pid_, SIGKILL);
            waitpid(pid_, &status, 0);
        }
        pid_ = -1;
    }
    if (master_ >= 0) {
        close(master_);
        master_ = -1;
    }
    if (reader_.joinable()) reader_.join();
}

void PtyAdapter::readLoop() {
    char buffer[8192];
    while (running_) {
        const auto count = read(master_, buffer, sizeof(buffer));
        if (count > 0) {
            publish("agent.message.delta", std::string(buffer, static_cast<std::size_t>(count)));
            continue;
        }
        if (count < 0 && errno == EINTR) continue;
        break;
    }
    if (running_.exchange(false)) publish("agent.finished", "");
}

void PtyAdapter::publish(std::string type, std::string content) {
    if (sink_) sink_({eventId(), context_.taskId, context_.runId, std::move(type), name_, std::move(content), now()});
}

}
