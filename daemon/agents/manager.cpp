#include "daemon/agents/manager.h"

#include "daemon/agents/codex_adapter.h"
#include "daemon/agents/event_id.h"
#include "daemon/agents/pty_adapter.h"
#include "daemon/repository/git.h"

#include <chrono>
#include <cctype>
#include <cstdlib>
#include <iostream>
#include <unistd.h>

namespace aegis::daemon::agents {
namespace {

std::int64_t now() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

bool isTerminal(const std::string &status) {
    return status == "completed" || status == "failed" || status == "interrupted" || status == "terminated";
}

bool validPrompt(const std::string &prompt) {
    if (prompt.empty() || prompt.size() > 65536) return false;
    if (prompt.find('\0') != std::string::npos) return false;
    for (const char c : prompt)
        if (!std::isspace(static_cast<unsigned char>(c))) return true;
    return false;
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

bool Manager::hasWorkingRunExcept(const std::string &sourceRunId) const {
    std::map<std::string, std::shared_ptr<Adapter>> snapshot;
    {
        std::lock_guard lock(mutex_);
        snapshot = active_;
    }
    for (const auto &[runId, adapter] : snapshot) {
        if (runId == sourceRunId || !adapter) continue;
        bool active;
        {
            std::lock_guard lock(provenanceMutex_);
            active = running_.contains(runId);
        }
        if (!active) {
            const auto run = store_.run(runId);
            if (!run || (run->status != "running" && run->status != "starting")) continue;
        }
        if (adapter->hasActiveTurn()) return true;
    }
    return false;
}

bool Manager::isTurnBusy(const std::string &runId) const {
    std::shared_ptr<Adapter> adapter;
    {
        std::lock_guard lock(mutex_);
        const auto found = active_.find(runId);
        if (found == active_.end()) return false;
        adapter = found->second;
    }
    return adapter->hasActiveTurn();
}

std::optional<AgentRun> Manager::launch(const std::string &taskId, const std::string &agent,
                                        const std::optional<std::string> &initialPrompt,
                                        const std::optional<std::string> &model,
                                        const std::optional<std::string> &continuationRequestId) {
    if (model) {
        if (agent != "codex" && agent != "claude" && agent != "opencode") return std::nullopt;
        const auto &value = *model;
        const auto plain = [](const char c) {
            return std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '.' || c == '/' || c == ':' || c == '-';
        };
        if (value.empty() || value.size() > 200 || value.find('\0') != std::string::npos ||
            !std::isalnum(static_cast<unsigned char>(value.front()))) return std::nullopt;
        for (const char c : value)
            if (!plain(c)) return std::nullopt;
    }
    if (initialPrompt) {
        if (agent != "codex" && agent != "claude" && agent != "opencode") return std::nullopt;
        if (!validPrompt(*initialPrompt)) return std::nullopt;
    }
    const auto task = store_.task(taskId);
    if (!task) return std::nullopt;
    if (task->repository.find('\0') != std::string::npos) return std::nullopt;
    {
        std::error_code error;
        if (!std::filesystem::equivalent(std::filesystem::path(task->repository), repository_, error) || error)
            return std::nullopt;
    }
    if (agent != "shell" && agent != "codex" && agent != "claude" && agent != "opencode") return std::nullopt;
    auto run = store_.startRun(taskId, agent, model);
    std::clog << "aegis_daemon: starting agent " << agent << " run " << run.id << '\n';
    if (continuationRequestId && !store_.linkContinuation(*continuationRequestId, run.id)) {
        store_.updateRunStatus(run.id, "failed");
        publish({newEventId(), taskId, run.id, "run.failed", agent, "continuation link failed", now()});
        return std::nullopt;
    }
    EventSink sink = [this](AgentEvent event) { publish(std::move(event)); };
    SessionSink sessionSink = [this](const std::string &runId, std::string sessionId) {
        store_.setExternalSessionId(runId, std::move(sessionId));
    };
    std::shared_ptr<Adapter> adapter;
    if (agent == "codex") adapter = std::make_shared<CodexAdapter>(std::move(sink), std::move(sessionSink));
    else if (agent == "shell") adapter = std::make_shared<PtyAdapter>(agent, std::vector<std::string>{"/bin/sh"}, std::move(sink));
    else if (agent == "claude")
        adapter = std::make_shared<PtyAdapter>(agent,
            initialPrompt ? (model ? std::vector<std::string>{agent, "--model", *model, "--", *initialPrompt}
                                   : std::vector<std::string>{agent, "--", *initialPrompt})
                          : (model ? std::vector<std::string>{agent, "--model", *model} : std::vector<std::string>{agent}), std::move(sink));
    else if (agent == "opencode")
        adapter = std::make_shared<PtyAdapter>(agent,
            initialPrompt ? (model ? std::vector<std::string>{agent, "--model", *model, "--prompt", *initialPrompt}
                                   : std::vector<std::string>{agent, "--prompt", *initialPrompt})
                          : (model ? std::vector<std::string>{agent, "--model", *model} : std::vector<std::string>{agent}), std::move(sink));
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
    publish({newEventId(), taskId, run.id, "run.started", agent, "", now()});
    {
        std::lock_guard lock(mutex_);
        active_[run.id] = adapter;
    }
    const auto context = RunContext{taskId, run.id, agent, repository_, run.externalSessionId, model};
    if (!adapter->start(context)) {
        std::cerr << "aegis_daemon: agent " << agent << " failed to start for run " << run.id << '\n';
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
        publish({newEventId(), taskId, run.id, "run.failed", agent, "agent adapter failed to start", now()});
        if (continuationRequestId) failContinuation(*continuationRequestId);
        return std::nullopt;
    }
    if (initialPrompt) {
        if (agent == "codex") {
            if (send(run.id, *initialPrompt) != SendResult::accepted) {
                terminate(run.id);
                if (continuationRequestId) failContinuation(*continuationRequestId);
                return std::nullopt;
            }
        } else {
            publish({newEventId(), taskId, run.id, "user.message", agent, *initialPrompt, now()});
        }
    }
    if (continuationRequestId && !store_.completeContinuation(*continuationRequestId, run.id))
        return std::nullopt;
    return store_.run(run.id);
}

bool Manager::linkContinuation(const std::string &requestId, const std::string &runId) {
    return store_.linkContinuation(requestId, runId);
}

bool Manager::failContinuation(const std::string &requestId) {
    return store_.failContinuation(requestId);
}

std::optional<std::string> Manager::continuationState(const std::string &requestId) const {
    return store_.continuationState(requestId);
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
        AgentEvent event{newEventId(), run->taskId, runId, "user.message", run->agent, std::string(message), now()};
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
    std::clog << "aegis_daemon: terminated agent run " << runId << '\n';
    {
        std::lock_guard lock(provenanceMutex_);
        running_.erase(runId);
        initialGitStatus_.erase(runId);
    }
    if (const auto run = store_.run(runId); run && !isTerminal(run->status)) {
        publish({newEventId(), run->taskId, runId, "run.terminated", run->agent, "", now()});
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
            std::clog << "aegis_daemon: run " << event.runId << " " << event.type.substr(4) << '\n';
            const auto status = event.type.substr(std::string("run.").size());
            store_.updateRunStatus(event.runId, status);
            std::lock_guard lock(provenanceMutex_);
            running_.erase(event.runId);
        }
        store_.appendEvent(event);
        if (event.type == "terminal.output") terminalEvents_.publish(event);
        else events_.publish(event);
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
        AgentEvent fileEvent{newEventId(), event.taskId, event.runId, "file.changed", event.agent, change.path, now()};
        store_.appendEvent(fileEvent);
        events_.publish(fileEvent);
    }
}

}
