#include "daemon/agents/codex_adapter.h"

#include "daemon/process/process.h"

#include <nlohmann/json.hpp>

#include <chrono>
#include <sstream>
#include <vector>

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

std::optional<AgentEvent> parseCodexJsonLine(std::string_view line,
                                             const std::string &taskId,
                                             const std::string &runId,
                                             const std::string &agent) {
    try {
        const auto object = nlohmann::json::parse(line);
        const auto type = object.value("type", std::string{});
        if (type == "thread.started") return AgentEvent{eventId(), taskId, runId, "agent.started", agent, object.value("thread_id", std::string{}), now()};
        if (type == "error") return AgentEvent{eventId(), taskId, runId, "agent.failed", agent, object.value("message", std::string{"Codex error"}), now()};
        if (type != "item.completed") return std::nullopt;
        const auto item = object.value("item", nlohmann::json::object());
        const auto itemType = item.value("type", std::string{});
        if (itemType == "agent_message")
            return AgentEvent{eventId(), taskId, runId, "agent.message.completed", agent, item.value("text", std::string{}), now()};
        if (itemType == "command_execution")
            return AgentEvent{eventId(), taskId, runId, "command.completed", agent, item.value("command", std::string{}), now()};
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

CodexAdapter::CodexAdapter(EventSink sink) : sink_(std::move(sink)) {}

CodexAdapter::~CodexAdapter() {
    terminate();
}

bool CodexAdapter::start(const RunContext &context) {
    terminate();
    context_ = context;
    running_ = true;
    if (sink_) sink_({eventId(), context_.taskId, context_.runId, "agent.started", "codex", "", now()});
    return true;
}

void CodexAdapter::send(std::string_view message) {
    if (!running_) return;
    if (worker_.joinable()) worker_.join();
    const auto prompt = std::string(message);
    worker_ = std::thread([this, prompt] {
        constexpr auto timeout = std::chrono::minutes(10);
        const auto result = process::run({"codex", "exec", "--json", "--color", "never", prompt}, context_.repository, timeout);
        for (const auto &event : parseCodexJsonOutput(result.output, context_.taskId, context_.runId, "codex"))
            if (sink_) sink_(event);
        if (sink_) sink_({eventId(), context_.taskId, context_.runId,
                          result.exitCode == 0 ? "agent.finished" : "agent.failed", "codex",
                          result.exitCode == 0 ? "" : result.timedOut
                              ? "Codex timed out after 10 minutes"
                              : "Codex exited with status " + std::to_string(result.exitCode), now()});
    });
}

void CodexAdapter::interrupt() {}

void CodexAdapter::terminate() {
    running_ = false;
    if (worker_.joinable()) worker_.join();
}

}
