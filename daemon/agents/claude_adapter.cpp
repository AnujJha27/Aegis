#include "daemon/agents/claude_adapter.h"

#include "daemon/agents/claude_protocol.h"
#include "daemon/agents/event_id.h"
#include "daemon/process/process.h"

#include <nlohmann/json.hpp>

#include <chrono>

namespace aegis::daemon::agents {

ClaudeAdapter::ClaudeAdapter(EventSink sink, SessionSink sessionSink)
    : sink_(std::move(sink)), sessionSink_(std::move(sessionSink)) {}

ClaudeAdapter::~ClaudeAdapter() { terminate(); }

bool ClaudeAdapter::start(const RunContext &context) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (running_ || stopping_) return false;
        stopping_ = true;
    }
    terminate();

    std::vector<std::string> argv{
        "claude", "--print", "--input-format", "stream-json", "--output-format",
        "stream-json", "--verbose", "--permission-prompt-tool", "stdio",
        "--permission-mode", "default"};
    if (context.externalSessionId) argv.push_back("--resume");
    if (context.externalSessionId) argv.push_back(*context.externalSessionId);
    if (context.model) argv.push_back("--model");
    if (context.model) argv.push_back(*context.model);

    auto child = std::make_shared<process::ChildProcess>();
    if (!child->start(argv, context.repository, true)) {
        std::lock_guard<std::mutex> lock(mutex_);
        stopping_ = false;
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        context_ = context;
        child_ = child;
        running_ = true;
        stopping_ = false;
        initialized_ = false;
        busy_ = false;
        interrupted_ = false;
        pendingPrompt_.clear();
        permissions_.clear();
    }

    reader_ = std::thread(&ClaudeAdapter::readLoop, this);
    writer_ = std::thread(&ClaudeAdapter::writerLoop, this);

    const nlohmann::json init{
        {"type", "control_request"},
        {"request_id", "aegis-initialize"},
        {"request", nlohmann::json{{"subtype", "initialize"}}}};
    if (!child->writeInput(init.dump() + "\n")) {
        child->terminate();
        {
            std::lock_guard<std::mutex> lock(mutex_);
            running_ = false;
        }
        cv_.notify_all();
        return false;
    }
    {
        std::unique_lock<std::mutex> lock(mutex_);
        const bool ready = cv_.wait_for(lock, std::chrono::seconds(20),
                                        [this] { return initialized_ || !running_; });
        if (!ready || !initialized_) {
            running_ = false;
            lock.unlock();
            child->terminate();
            publish("run.failed", "agent initialization timed out or failed");
            return false;
        }
    }
    return true;
}

SendResult ClaudeAdapter::send(std::string_view message) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!running_ || stopping_) return SendResult::unavailable;
        if (busy_) return SendResult::busy;
        pendingPrompt_.assign(message);
        busy_ = true;
        interrupted_ = false;
    }
    cv_.notify_all();
    return SendResult::accepted;
}

void ClaudeAdapter::writerLoop() {
    while (true) {
        std::unique_lock<std::mutex> lock(mutex_);
        cv_.wait(lock, [this] { return stopping_ || !running_ || (initialized_ && !pendingPrompt_.empty()); });
        if (stopping_ || !running_) return;
        lock.unlock();
        dispatchPending();
    }
}

void ClaudeAdapter::dispatchPending() {
    std::string prompt;
    RunContext context;
    std::shared_ptr<process::ChildProcess> child;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!initialized_ || !running_ || stopping_ || pendingPrompt_.empty()) return;
        prompt = std::move(pendingPrompt_);
        pendingPrompt_.clear();
        context = context_;
        child = child_;
    }
    publish("turn.started");
    const nlohmann::json envelope{{"type", "user"},
                                  {"message", nlohmann::json{{"role", "user"}, {"content", prompt}}},
                                  {"parent_tool_use_id", nullptr},
                                  {"session_id", context.externalSessionId.value_or("")},
                                  {"client_composed", true}};
    if (!child->writeInput(envelope.dump() + "\n")) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            busy_ = false;
            running_ = false;
        }
        child->terminate();
        publish("run.failed", "failed to write prompt to agent process");
    }
}

bool ClaudeAdapter::hasActiveTurn() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return busy_;
}

void ClaudeAdapter::publish(std::string type, std::string content) {
    RunContext context;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        context = context_;
    }
    AgentEvent event;
    event.id = newEventId();
    event.taskId = context.taskId;
    event.runId = context.runId;
    event.type = std::move(type);
    event.agent = context.agent;
    event.content = std::move(content);
    event.timestamp = std::chrono::duration_cast<std::chrono::milliseconds>(
                          std::chrono::system_clock::now().time_since_epoch())
                          .count();
    emitEvent(sink_, std::move(event));
}

void ClaudeAdapter::readLoop() {
    auto child = child_;
    if (!child) return;
    std::string pending;
    auto onOutput = [&](std::string_view chunk) {
        pending.append(chunk);
        std::size_t pos;
        while ((pos = pending.find('\n')) != std::string::npos) {
            std::string line = pending.substr(0, pos);
            pending.erase(0, pos + 1);
            if (!line.empty()) handleLine(line);
        }
        if (pending.size() > 1024 * 1024) {
            pending.clear();
            child->terminate();
        }
    };
    child->wait(std::chrono::hours(24), onOutput, false);
    bool emitFailure;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        emitFailure = running_ && !stopping_;
        running_ = false;
        busy_ = false;
        permissions_.clear();
    }
    if (emitFailure) publish("run.failed", "agent process exited unexpectedly");
    cv_.notify_all();
}

void ClaudeAdapter::terminate() {
    std::shared_ptr<process::ChildProcess> child;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stopping_ = true;
        running_ = false;
        child = child_;
    }
    if (child) child->terminate();
    cv_.notify_all();
    if (writer_.joinable()) writer_.join();
    if (reader_.joinable()) reader_.join();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        child_.reset();
        pendingPrompt_.clear();
        permissions_.clear();
        busy_ = false;
        initialized_ = false;
        interrupted_ = false;
        stopping_ = false;
    }
}

void ClaudeAdapter::interrupt() {
    bool hadQueuedPrompt = false;
    std::shared_ptr<process::ChildProcess> child;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!busy_) return;
        if (!pendingPrompt_.empty()) {
            pendingPrompt_.clear();
            busy_ = false;
            hadQueuedPrompt = true;
        } else {
            interrupted_ = true;
            child = child_;
        }
    }
    if (hadQueuedPrompt) {
        publish("turn.interrupted");
        return;
    }
    if (!child) return;
    const nlohmann::json envelope{{"type", "control_request"},
                                  {"request_id", newEventId()},
                                  {"request", nlohmann::json{{"subtype", "interrupt"}}}};
    child->writeInput(envelope.dump() + "\n");
}

std::vector<AgentEvent> ClaudeAdapter::pendingPermissions() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<AgentEvent> result;
    if (!running_ || stopping_) return result;
    for (const auto &entry : permissions_) result.push_back(entry.second);
    return result;
}

bool ClaudeAdapter::respondPermission(std::string_view requestId, bool allow) {
    const std::string id(requestId);
    std::shared_ptr<process::ChildProcess> child;
    std::string payload;
    {
        std::unique_lock<std::mutex> lock(mutex_);
        if (!running_ || stopping_) return false;
        auto it = permissions_.find(id);
        if (it == permissions_.end()) return false;
        child = child_;
        if (!child) return false;
        nlohmann::json content;
        try {
            content = nlohmann::json::parse(it->second.content);
        } catch (...) {
            return false;
        }
        const nlohmann::json originalInput = content.value("input", nlohmann::json::object());
        nlohmann::json decision{{"behavior", allow ? "allow" : "deny"}};
        if (allow) {
            decision["updatedInput"] = originalInput;
        } else {
            decision["message"] = "Denied in Aegis";
        }
        const nlohmann::json envelope{{"type", "control_response"},
                                      {"response", nlohmann::json{{"subtype", "success"},
                                                                   {"request_id", id},
                                                                   {"response", decision}}}};
        payload = envelope.dump() + "\n";
        if (!child->writeInput(payload)) {
            running_ = false;
            busy_ = false;
            permissions_.clear();
            lock.unlock();
            child->terminate();
            cv_.notify_all();
            publish("run.failed", "failed to write permission decision to agent process");
            return false;
        }
        permissions_.erase(it);
    }
    const nlohmann::json resolved{{"request_id", id}, {"allowed", allow}};
    publish("permission.resolved", resolved.dump());
    return true;
}

void ClaudeAdapter::handleLine(std::string_view line) {
    nlohmann::json parsed;
    try {
        parsed = nlohmann::json::parse(line);
    } catch (...) {
        return;
    }
    if (!parsed.is_object()) return;
    if (!parsed.contains("type") || !parsed.at("type").is_string()) return;
    const std::string type = parsed.value("type", std::string{});
    if (type == "control_response") {
        try {
            const auto &response = parsed.value("response", nlohmann::json::object());
            const std::string requestId = response.value("request_id", std::string{});
            if (requestId != "aegis-initialize") return;
            if (response.value("subtype", std::string{}) == "success") {
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    initialized_ = true;
                }
                cv_.notify_all();
            } else {
                std::shared_ptr<process::ChildProcess> child;
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    running_ = false;
                    busy_ = false;
                    child = child_;
                }
                if (child) child->terminate();
                cv_.notify_all();
                publish("run.failed", response.value("error", std::string{"agent initialization failed"}));
            }
        } catch (const nlohmann::json::exception &) {
            return;
        }
        return;
    }
    if (type == "control_cancel_request") {
        try {
            const std::string requestId = parsed.value("request_id", std::string{});
            {
                std::lock_guard<std::mutex> lock(mutex_);
                permissions_.erase(requestId);
            }
            publish("permission.cancelled", requestId);
        } catch (const nlohmann::json::exception &) {
            return;
        }
        return;
    }

    RunContext context;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        context = context_;
    }
    try {
    for (auto &event : parseClaudeJsonLine(line, context)) {
        if (event.type == "run.session") {
            const std::string sessionId = parsed.value("session_id", std::string{});
            {
                std::lock_guard<std::mutex> lock(mutex_);
                context_.externalSessionId = sessionId;
            }
            notifySession(sessionSink_, context.runId, sessionId);
            continue;
        }
        if (event.type == "permission.requested") {
            const std::string requestId = parsed.value("request_id", std::string{});
            {
                std::lock_guard<std::mutex> lock(mutex_);
                permissions_[requestId] = event;
            }
            emitEvent(sink_, event);
            continue;
        }
        if (event.type == "turn.completed") {
            bool interrupted;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                busy_ = false;
                permissions_.clear();
                interrupted = interrupted_;
                interrupted_ = false;
            }
            if (interrupted) event.type = "turn.interrupted";
            emitEvent(sink_, std::move(event));
            continue;
        }
        if (event.type == "control.unsupported") {
            const std::string unsupportedId = event.content;
            if (unsupportedId.empty()) {
                std::shared_ptr<process::ChildProcess> child;
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    running_ = false;
                    busy_ = false;
                    permissions_.clear();
                    child = child_;
                }
                if (child) child->terminate();
                publish("run.failed", "agent sent unsupported control request without id");
            } else {
                std::shared_ptr<process::ChildProcess> child;
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    child = child_;
                }
                const nlohmann::json envelope{{"type", "control_response"},
                                              {"response", nlohmann::json{{"subtype", "error"},
                                                                           {"request_id", unsupportedId},
                                                                           {"error", "unsupported control request"}}}};
                if (child) child->writeInput(envelope.dump() + "\n");
            }
            continue;
        }
        if (event.type == "run.failed") {
            bool interrupted;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                interrupted = interrupted_;
            }
            if (interrupted) {
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    busy_ = false;
                    permissions_.clear();
                    interrupted_ = false;
                }
                event.type = "turn.interrupted";
                emitEvent(sink_, std::move(event));
                continue;
            }
            std::shared_ptr<process::ChildProcess> child;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                running_ = false;
                busy_ = false;
                permissions_.clear();
                child = child_;
            }
            if (child) child->terminate();
            emitEvent(sink_, std::move(event));
            continue;
        }
        emitEvent(sink_, std::move(event));
    }
    } catch (const nlohmann::json::exception &) {
        return;
    }
}

}  // namespace aegis::daemon::agents
