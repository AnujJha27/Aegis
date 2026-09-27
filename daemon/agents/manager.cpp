#include "daemon/agents/manager.h"

#include "daemon/agents/codex_adapter.h"
#include "daemon/agents/pty_adapter.h"
#include "daemon/repository/git.h"

#include <chrono>
#include <cstdlib>
#include <atomic>
#include <unistd.h>

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

bool isTerminal(const std::string &status) {
    return status == "completed" || status == "failed" || status == "interrupted" || status == "terminated";
}

}

Manager::Manager(std::filesystem::path repository, Store &store, EventHub &events)
    : repository_(std::move(repository)), store_(store), events_(events) {}

Manager::~Manager() {
    std::vector<std::string> runs;
    {
        std::lock_guard lock(mutex_);
        for (const auto &[runId, adapter] : active_) runs.push_back(runId);
    }
    for (const auto &runId : runs) terminate(runId);
}

std::vector<AgentInfo> Manager::available() const {
    const auto executable = [](const std::string &name) {
        const char *path = std::getenv("PATH");
        if (!path) return false;
        std::string paths(path);
        std::size_t start = 0;
        while (start <= paths.size()) {
            const auto end = paths.find(':', start);
            const auto directory = paths.substr(start, end == std::string::npos ? std::string::npos : end - start);
            if (access((directory + "/" + name).c_str(), X_OK) == 0) return true;
            if (end == std::string::npos) break;
            start = end + 1;
        }
        return false;
    };
    return {{"shell", true, {false, true, true, true}},
            {"codex", executable("codex"), {true, false, true, true}},
            {"claude", executable("claude"), {false, true, true, true}},
            {"opencode", executable("opencode"), {false, true, true, true}}};
}

bool Manager::hasRunningRuns() const {
    std::lock_guard lock(provenanceMutex_);
    return !running_.empty();
}

bool Manager::isRunning(const std::string &runId) const {
    std::lock_guard lock(provenanceMutex_);
    return running_.contains(runId);
}

std::optional<AgentRun> Manager::launch(const std::string &taskId, const std::string &agent) {
    if (!store_.task(taskId)) return std::nullopt;
    if (agent != "shell" && agent != "codex" && agent != "claude" && agent != "opencode") return std::nullopt;
    auto run = store_.startRun(taskId, agent);
    EventSink sink = [this](AgentEvent event) { publish(std::move(event)); };
    SessionSink sessionSink = [this](const std::string &runId, std::string sessionId) {
        store_.setExternalSessionId(runId, std::move(sessionId));
    };
    std::shared_ptr<Adapter> adapter;
    if (agent == "codex") adapter = std::make_shared<CodexAdapter>(std::move(sink), std::move(sessionSink));
    else if (agent == "shell") adapter = std::make_shared<PtyAdapter>(agent, std::vector<std::string>{"/bin/sh"}, std::move(sink));
    else adapter = std::make_shared<PtyAdapter>(agent, std::vector<std::string>{agent}, std::move(sink));

    repository::GitRepository git(repository_);
    std::map<std::string, std::string> baseline;
    for (const auto &change : git.changes()) baseline[change.path] = change.indexStatus + change.worktreeStatus;
    {
        std::lock_guard lock(provenanceMutex_);
        initialGitStatus_[run.id] = std::move(baseline);
        running_.insert(run.id);
    }
    store_.updateRunStatus(run.id, "running");
    publish({eventId(), taskId, run.id, "run.started", agent, "", now()});
    {
        std::lock_guard lock(mutex_);
        active_[run.id] = adapter;
    }
    const auto context = RunContext{taskId, run.id, agent, repository_, run.externalSessionId};
    if (!adapter->start(context)) {
        {
            std::lock_guard lock(mutex_);
            active_.erase(run.id);
        }
        {
            std::lock_guard lock(provenanceMutex_);
            initialGitStatus_.erase(run.id);
            running_.erase(run.id);
        }
        store_.updateRunStatus(run.id, "failed");
        publish({eventId(), taskId, run.id, "run.failed", agent, "agent adapter failed to start", now()});
        return std::nullopt;
    }
    return store_.run(run.id);
}

SendResult Manager::send(const std::string &runId, std::string_view message) {
    std::lock_guard eventLock(eventMutex_);
    std::shared_ptr<Adapter> adapter;
    {
        std::lock_guard lock(mutex_);
        const auto found = active_.find(runId);
        if (found == active_.end()) return SendResult::unavailable;
        adapter = found->second;
    }
    const auto result = adapter->send(message);
    if (result != SendResult::accepted) return result;
    if (const auto run = store_.run(runId)) {
        AgentEvent event{eventId(), run->taskId, runId, "user.message", run->agent, std::string(message), now()};
        store_.appendEvent(event);
        events_.publish(event);
    }
    return result;
}

bool Manager::sendPty(const std::string &runId, std::string_view input) {
    std::shared_ptr<Adapter> adapter;
    {
        std::lock_guard lock(mutex_);
        const auto found = active_.find(runId);
        if (found == active_.end()) return false;
        adapter = found->second;
    }
    return adapter->sendPty(input);
}

bool Manager::resizePty(const std::string &runId, unsigned short cols, unsigned short rows) {
    std::shared_ptr<Adapter> adapter;
    {
        std::lock_guard lock(mutex_);
        const auto found = active_.find(runId);
        if (found == active_.end()) return false;
        adapter = found->second;
    }
    return adapter->resizePty(cols, rows);
}

bool Manager::interrupt(const std::string &runId) {
    std::shared_ptr<Adapter> adapter;
    {
        std::lock_guard lock(mutex_);
        const auto found = active_.find(runId);
        if (found == active_.end()) return false;
        adapter = found->second;
    }
    adapter->interrupt();
    return true;
}

bool Manager::terminate(const std::string &runId) {
    std::shared_ptr<Adapter> adapter;
    {
        std::lock_guard lock(mutex_);
        const auto found = active_.find(runId);
        if (found == active_.end()) return false;
        adapter = std::move(found->second);
        active_.erase(found);
    }
    adapter->terminate();
    {
        std::lock_guard lock(provenanceMutex_);
        running_.erase(runId);
        initialGitStatus_.erase(runId);
    }
    if (const auto run = store_.run(runId); run && !isTerminal(run->status)) {
        publish({eventId(), run->taskId, runId, "run.terminated", run->agent, "", now()});
    }
    return true;
}

void Manager::publish(AgentEvent event) {
    std::map<std::string, std::string> baseline;
    bool hasBaseline = false;
    {
        std::lock_guard eventLock(eventMutex_);
        if (event.type == "run.session") {
            store_.setExternalSessionId(event.runId, event.content);
            return;
        }
        if (event.type == "run.completed" || event.type == "run.failed" || event.type == "run.interrupted" || event.type == "run.terminated") {
            const auto status = event.type.substr(std::string("run.").size());
            store_.updateRunStatus(event.runId, status);
            std::lock_guard lock(provenanceMutex_);
            running_.erase(event.runId);
        }
        store_.appendEvent(event);
        events_.publish(event);
        if (event.type == "run.completed" || event.type == "run.failed") {
            std::lock_guard lock(provenanceMutex_);
            const auto found = initialGitStatus_.find(event.runId);
            if (found != initialGitStatus_.end()) {
                baseline = std::move(found->second);
                initialGitStatus_.erase(found);
                hasBaseline = true;
            }
        }
    }
    if (event.type != "run.completed" && event.type != "run.failed") return;
    if (!hasBaseline) return;
    repository::GitRepository git(repository_);
    for (const auto &change : git.changes()) {
        const auto current = change.indexStatus + change.worktreeStatus;
        const auto previous = baseline.find(change.path);
        if (previous != baseline.end() && previous->second == current) continue;
        AgentEvent fileEvent{eventId(), event.taskId, event.runId, "file.changed", event.agent, change.path, now()};
        store_.appendEvent(fileEvent);
        events_.publish(fileEvent);
    }
}

}
