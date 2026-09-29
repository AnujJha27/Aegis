#include "daemon/agents/codex_adapter.h"

#include "daemon/agents/event_id.h"
#include "daemon/process/process.h"

#include <nlohmann/json.hpp>

#include <chrono>
#include <sstream>

namespace aegis::daemon::agents {
namespace {

std::int64_t now() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

}

std::optional<AgentEvent> parseCodexJsonLine(std::string_view line,
                                             const std::string &taskId,
                                             const std::string &runId,
                                             const std::string &agent) {
    try {
        const auto object = nlohmann::json::parse(line);
        const auto type = object.value("type", std::string{});
        if (type == "thread.started") {
            const auto threadId = object.value("thread_id", std::string{});
            if (!threadId.empty()) return AgentEvent{newEventId(), taskId, runId, "run.session", agent, threadId, now()};
            return std::nullopt;
        }
        if (type == "error")
            return AgentEvent{newEventId(), taskId, runId, "run.failed", agent, object.value("message", std::string{"Codex error"}), now()};
        if (type != "item.started" && type != "item.completed") return std::nullopt;
        const auto item = object.value("item", nlohmann::json::object());
        const auto itemType = item.value("type", std::string{});
        if (type == "item.completed" && itemType == "agent_message")
            return AgentEvent{newEventId(), taskId, runId, "agent.message.completed", agent, item.value("text", std::string{}), now()};
        if (itemType == "command_execution")
            return AgentEvent{newEventId(), taskId, runId,
                              type == "item.started" ? "command.started" : "command.completed",
                              agent, item.value("command", std::string{}), now()};
    } catch (const nlohmann::json::exception &) {
        return std::nullopt;
    }
    return std::nullopt;
}

std::vector<AgentEvent> parseCodexJsonOutput(std::string_view output,
                                             const std::string &taskId,
                                             const std::string &runId,
                                             const std::string &agent) {
    std::istringstream lines{std::string(output)};
    std::vector<AgentEvent> events;
    std::string line;
    while (std::getline(lines, line))
        if (auto event = parseCodexJsonLine(line, taskId, runId, agent)) events.push_back(std::move(*event));
    return events;
}

CodexAdapter::CodexAdapter(EventSink sink, SessionSink sessionSink)
    : sink_(std::move(sink)), sessionSink_(std::move(sessionSink)) {}

CodexAdapter::~CodexAdapter() {
    terminate();
}

bool CodexAdapter::start(const RunContext &context) {
    terminate();
    {
        std::lock_guard lock(mutex_);
        context_ = context;
        pendingPrompt_.clear();
        hasPendingPrompt_ = false;
        stopping_ = false;
        interrupted_ = false;
        child_.reset();
        busy_ = false;
        running_ = true;
    }
    worker_ = std::thread([this] { workerLoop(); });
    return true;
}

SendResult CodexAdapter::send(std::string_view message) {
    std::lock_guard lock(mutex_);
    if (!running_ || stopping_) return SendResult::unavailable;
    if (busy_) return SendResult::busy;
    interrupted_ = false;
    pendingPrompt_ = std::string(message);
    hasPendingPrompt_ = true;
    busy_ = true;
    condition_.notify_one();
    return SendResult::accepted;
}

void CodexAdapter::interrupt() {
    std::shared_ptr<process::ChildProcess> child;
    {
        std::lock_guard lock(mutex_);
        if (!busy_) return;
        interrupted_ = true;
        child = child_;
    }
    if (child) child->interrupt();
}

void CodexAdapter::terminate() {
    std::shared_ptr<process::ChildProcess> child;
    {
        std::lock_guard lock(mutex_);
        if (!worker_.joinable()) { running_ = false; return; }
        stopping_ = true;
        running_ = false;
        child = child_;
    }
    if (child) child->terminate();
    condition_.notify_all();
    if (worker_.joinable()) worker_.join();
    std::lock_guard lock(mutex_);
    child_.reset();
    hasPendingPrompt_ = false;
    busy_ = false;
}

void CodexAdapter::workerLoop() {
    for (;;) {
        std::string prompt;
        RunContext context;
        {
            std::unique_lock lock(mutex_);
            condition_.wait(lock, [&] { return stopping_ || hasPendingPrompt_; });
            if (stopping_) return;
            prompt = std::move(pendingPrompt_);
            pendingPrompt_.clear();
            hasPendingPrompt_ = false;
            context = context_;
        }

        publish({newEventId(), context.taskId, context.runId, "turn.started", context.agent, "", now()});
        std::vector<std::string> command{"codex", "exec"};
        if (context.externalSessionId) {
            command.push_back("resume");
            command.push_back("--json");
            command.push_back(*context.externalSessionId);
            command.push_back(prompt);
        } else {
            command.insert(command.end(), {"--json", "--color", "never", prompt});
        }

        auto child = std::make_shared<process::ChildProcess>();
        {
            std::lock_guard lock(mutex_);
            child_ = child;
        }

        std::string pendingLine;
        std::string failure;
        bool started = child->start(command, context.repository);
        process::Result result;
        if (started) {
            bool interruptedBeforeStart;
            bool stoppingBeforeStart;
            {
                std::lock_guard lock(mutex_);
                interruptedBeforeStart = interrupted_;
                stoppingBeforeStart = stopping_;
            }
            if (stoppingBeforeStart) child->terminate();
            else if (interruptedBeforeStart) child->interrupt();
            result = child->wait(std::chrono::minutes(10), [&](std::string_view chunk) {
                pendingLine.append(chunk);
                std::size_t newline;
                while ((newline = pendingLine.find('\n')) != std::string::npos) {
                    auto line = pendingLine.substr(0, newline);
                    pendingLine.erase(0, newline + 1);
                    if (auto event = parseCodexJsonLine(line, context.taskId, context.runId, context.agent)) {
                        if (event->type == "run.session") {
                            {
                                std::lock_guard lock(mutex_);
                                context_.externalSessionId = event->content;
                            }
                            notifySession(sessionSink_, context.runId, event->content);
                        } else if (event->type == "run.failed") failure = event->content;
                        else publish(std::move(*event));
                    }
                }
                if (pendingLine.size() > 1024 * 1024) pendingLine.clear();
            }, false);
            if (!pendingLine.empty()) {
                if (auto event = parseCodexJsonLine(pendingLine, context.taskId, context.runId, context.agent)) {
                    if (event->type == "run.session") {
                        {
                            std::lock_guard lock(mutex_);
                            context_.externalSessionId = event->content;
                        }
                        notifySession(sessionSink_, context.runId, event->content);
                    } else if (event->type == "run.failed") failure = event->content;
                    else publish(std::move(*event));
                }
            }
        }

        bool stopping;
        bool interrupted;
        bool resumable;
        {
            std::lock_guard lock(mutex_);
            child_.reset();
            stopping = stopping_;
            interrupted = interrupted_;
            resumable = context_.externalSessionId.has_value();
            busy_ = false;
        }
        if (stopping) return;
        if (interrupted && resumable) publish({newEventId(), context.taskId, context.runId, "turn.interrupted", context.agent, "", now()});
        else if (interrupted) {
            running_ = false;
            publish({newEventId(), context.taskId, context.runId, "run.interrupted", context.agent, "Codex interrupted before a resumable session was available", now()});
            return;
        }
        else if (!started || result.exitCode != 0 || !failure.empty()) {
            running_ = false;
            const auto message = !started ? "Could not start Codex" : !failure.empty() ? failure :
                result.timedOut ? "Codex timed out after 10 minutes" : "Codex exited with status " + std::to_string(result.exitCode);
            publish({newEventId(), context.taskId, context.runId, "run.failed", context.agent, message, now()});
            return;
        } else publish({newEventId(), context.taskId, context.runId, "turn.completed", context.agent, "", now()});
    }
}

void CodexAdapter::publish(AgentEvent event) {
    emitEvent(sink_, std::move(event));
}

}
