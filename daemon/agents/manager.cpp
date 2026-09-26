#include "daemon/agents/manager.h"

#include "daemon/agents/codex_adapter.h"
#include "daemon/agents/pty_adapter.h"
#include "daemon/repository/git.h"

#include <chrono>
#include <cstdlib>
#include <atomic>
#include <unistd.h>

namespace aegis::daemon::agents {

Manager::Manager(std::filesystem::path repository, Store &store, EventHub &events)
    : repository_(std::move(repository)), store_(store), events_(events) {}

Manager::~Manager() {
    std::lock_guard lock(mutex_);
    for (auto &[runId, adapter] : active_) adapter->terminate();
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
            std::string candidate = directory;
            candidate += "/" + name;
            if (access(candidate.c_str(), X_OK) == 0) return true;
            if (end == std::string::npos) break;
            start = end + 1;
        }
        return false;
    };
    return {{"shell", true, {false, true}},
            {"codex", executable("codex"), {true, false}},
            {"claude", executable("claude"), {false, true}},
            {"opencode", executable("opencode"), {false, true}}};
}

bool Manager::hasRunningRuns() const {
    std::lock_guard lock(provenanceMutex_);
    return !running_.empty();
}

std::optional<AgentRun> Manager::launch(const std::string &taskId, const std::string &agent) {
    if (agent != "shell" && agent != "codex" && agent != "claude" && agent != "opencode") return std::nullopt;
    const auto run = store_.startRun(taskId, agent);
    EventSink sink = [this](AgentEvent event) { publish(std::move(event)); };
    std::unique_ptr<Adapter> adapter;
    if (agent == "codex") adapter = std::make_unique<CodexAdapter>(std::move(sink));
    else if (agent == "shell") adapter = std::make_unique<PtyAdapter>(agent, std::vector<std::string>{"/bin/sh"}, std::move(sink));
    else adapter = std::make_unique<PtyAdapter>(agent, std::vector<std::string>{agent}, std::move(sink));
    repository::GitRepository git(repository_);
    std::map<std::string, std::string> baseline;
    for (const auto &change : git.changes()) baseline[change.path] = change.indexStatus + change.worktreeStatus;
    {
        std::lock_guard lock(provenanceMutex_);
        initialGitStatus_[run.id] = std::move(baseline);
        running_.insert(run.id);
    }
    if (!adapter->start({taskId, run.id, agent, repository_})) {
        std::lock_guard lock(provenanceMutex_);
        initialGitStatus_.erase(run.id);
        running_.erase(run.id);
        return std::nullopt;
    }
    std::lock_guard lock(mutex_);
    active_[run.id] = std::move(adapter);
    return run;
}

bool Manager::send(const std::string &runId, std::string_view message) {
    std::lock_guard lock(mutex_);
    const auto found = active_.find(runId);
    if (found == active_.end()) return false;
    found->second->send(message);
    return true;
}

bool Manager::sendPty(const std::string &runId, std::string_view input) {
    std::lock_guard lock(mutex_);
    const auto found = active_.find(runId);
    return found != active_.end() && found->second->sendPty(input);
}

bool Manager::interrupt(const std::string &runId) {
    std::lock_guard lock(mutex_);
    const auto found = active_.find(runId);
    if (found == active_.end()) return false;
    found->second->interrupt();
    return true;
}

bool Manager::terminate(const std::string &runId) {
    std::unique_ptr<Adapter> adapter;
    {
        std::lock_guard lock(mutex_);
        const auto found = active_.find(runId);
        if (found == active_.end()) return false;
        adapter = std::move(found->second);
        active_.erase(found);
    }
    {
        std::lock_guard lock(provenanceMutex_);
        running_.erase(runId);
        initialGitStatus_.erase(runId);
    }
    adapter->terminate();
    return true;
}

void Manager::publish(AgentEvent event) {
    store_.appendEvent(event);
    events_.publish(event);
    if (event.type != "agent.finished" && event.type != "agent.failed") return;
    std::map<std::string, std::string> baseline;
    {
        std::lock_guard lock(provenanceMutex_);
        running_.erase(event.runId);
        const auto found = initialGitStatus_.find(event.runId);
        if (found == initialGitStatus_.end()) return;
        baseline = std::move(found->second);
        initialGitStatus_.erase(found);
    }
    repository::GitRepository git(repository_);
    static std::atomic_uint64_t sequence = 0;
    const auto timestamp = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    for (const auto &change : git.changes()) {
        const auto current = change.indexStatus + change.worktreeStatus;
        const auto previous = baseline.find(change.path);
        if (previous != baseline.end() && previous->second == current) continue;
        AgentEvent fileEvent{"file-" + std::to_string(timestamp) + "-" + std::to_string(++sequence), event.taskId, event.runId, "file.changed", event.agent, change.path, timestamp};
        store_.appendEvent(fileEvent);
        events_.publish(fileEvent);
    }
}

}
