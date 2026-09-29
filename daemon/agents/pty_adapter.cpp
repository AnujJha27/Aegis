#include "daemon/agents/pty_adapter.h"

#include "daemon/agents/event_id.h"

#include <cerrno>
#include <csignal>
#include <cstring>
#include <pty.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <unistd.h>

#include <chrono>
#include <algorithm>
#include <limits>

namespace aegis::daemon::agents {
namespace {

std::int64_t now() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

}

PtyAdapter::PtyAdapter(std::string name, std::vector<std::string> command, EventSink sink,
                       PtyOutputBatching batching)
    : name_(std::move(name)), command_(std::move(command)), sink_(std::move(sink)), batching_(batching) {
    if (batching_.flushInterval.count() < 1) batching_.flushInterval = std::chrono::milliseconds(1);
    batching_.maxBatchBytes = std::clamp<std::size_t>(batching_.maxBatchBytes, 1, 64 * 1024);
}

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
    std::string batch;
    batch.reserve(batching_.maxBatchBytes);
    auto deadline = std::chrono::steady_clock::time_point{};
    const auto flush = [&] {
        if (batch.empty()) return;
        publish("terminal.output", std::move(batch));
        batch.clear();
        batch.reserve(batching_.maxBatchBytes);
    };
    while (running_) {
        pollfd descriptor{master, POLLIN, 0};
        int timeout = -1;
        if (!batch.empty()) {
            const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now()).count();
            timeout = static_cast<int>(std::clamp<std::int64_t>(remaining, 0, std::numeric_limits<int>::max()));
        }
        const auto ready = poll(&descriptor, 1, timeout);
        if (ready < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (ready == 0) {
            flush();
            continue;
        }
        if (descriptor.revents & POLLIN) {
            char buffer[8192];
            const auto capacity = std::min(sizeof(buffer), batching_.maxBatchBytes - batch.size());
            const auto count = read(master, buffer, capacity);
            if (count > 0) {
                if (batch.empty()) deadline = std::chrono::steady_clock::now() + batching_.flushInterval;
                batch.append(buffer, static_cast<std::size_t>(count));
                if (batch.size() >= batching_.maxBatchBytes) flush();
                continue;
            }
            if (count < 0 && errno == EINTR) continue;
            if (count < 0 && errno != EIO) break;
        }
        if (descriptor.revents & (POLLERR | POLLHUP | POLLNVAL)) break;
    }
    flush();
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
    emitEvent(sink_, {newEventId(), context_.taskId, context_.runId, std::move(type), name_, std::move(content), now()});
}

}
