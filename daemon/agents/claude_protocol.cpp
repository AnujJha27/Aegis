#include "daemon/agents/claude_protocol.h"

#include "daemon/agents/event_id.h"

#include <nlohmann/json.hpp>

#include <chrono>
#include <string>

namespace aegis::daemon::agents {
namespace {

using json = nlohmann::json;

AgentEvent makeEvent(const RunContext &context, std::string type, std::string content) {
    AgentEvent event;
    event.id = newEventId();
    event.runId = context.runId;
    event.taskId = context.taskId;
    event.agent = context.agent;
    event.type = std::move(type);
    event.content = std::move(content);
    event.timestamp = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    return event;
}

std::string sessionEvent(const json &j) {
    if (!j.contains("session_id") || !j["session_id"].is_string()) return {};
    auto id = j["session_id"].get<std::string>();
    return id.empty() ? std::string{} : id;
}

void handleAssistant(const json &message, const RunContext &context, std::vector<AgentEvent> &out) {
    if (!message.contains("content") || !message["content"].is_array()) return;
    std::string text;
    for (const auto &block : message["content"]) {
        if (!block.is_object() || !block.contains("type") || !block["type"].is_string()) continue;
        const auto type = block["type"].get<std::string>();
        if (type == "text") {
            if (block.contains("parent_tool_use_id") && !block["parent_tool_use_id"].is_null()) continue;
            if (block.contains("text") && block["text"].is_string()) text += block["text"].get<std::string>();
        } else if (type == "tool_use") {
            json payload = json::object();
            payload["id"] = block.value("id", "");
            payload["name"] = block.value("name", "");
            payload["input"] = block.contains("input") && block["input"].is_object() ? block["input"] : json::object();
            out.push_back(makeEvent(context, "tool.started", payload.dump()));
        }
    }
    if (!text.empty()) out.push_back(makeEvent(context, "agent.message.completed", text));
}

void handleUser(const json &message, const RunContext &context, std::vector<AgentEvent> &out) {
    if (!message.contains("content") || !message["content"].is_array()) return;
    for (const auto &block : message["content"]) {
        if (!block.is_object() || !block.contains("type") || block["type"] != "tool_result") continue;
        json payload = json::object();
        payload["tool_use_id"] = block.value("tool_use_id", "");
        payload["content"] = block.contains("content") ? block["content"] : json(nullptr);
        payload["is_error"] = block.value("is_error", false);
        out.push_back(makeEvent(context, "tool.completed", payload.dump()));
    }
}

void handleResult(const json &j, const RunContext &context, std::vector<AgentEvent> &out) {
    if (auto sessionId = sessionEvent(j); !sessionId.empty())
        out.push_back(makeEvent(context, "run.session", sessionId));
    const bool isError = j.value("is_error", false);
    if (!isError) {
        out.push_back(makeEvent(context, "turn.completed",
                                j.contains("result") && j["result"].is_string() ? j["result"].get<std::string>() : ""));
        return;
    }
    std::string detail;
    if (j.contains("result") && j["result"].is_string()) detail = j["result"].get<std::string>();
    else if (j.contains("errors") && j["errors"].is_array()) {
        for (const auto &error : j["errors"]) {
            if (!detail.empty()) detail += "; ";
            detail += error.is_string() ? error.get<std::string>() : error.dump();
        }
    }
    out.push_back(makeEvent(context, "run.failed", detail));
}

void handleControlRequest(const json &j, const RunContext &context, std::vector<AgentEvent> &out) {
    const auto requestId = j.contains("request_id") && j["request_id"].is_string()
                               ? j["request_id"].get<std::string>() : std::string{};
    if (!j.contains("request") || !j["request"].is_object()) {
        out.push_back(makeEvent(context, "control.unsupported", requestId));
        return;
    }
    if (requestId.empty()) {
        out.push_back(makeEvent(context, "control.unsupported", ""));
        return;
    }
    const auto &request = j["request"];
    if (!request.contains("subtype") || request["subtype"] != "can_use_tool") {
        out.push_back(makeEvent(context, "control.unsupported", requestId));
        return;
    }
    if (!request.contains("tool_name") || !request["tool_name"].is_string() ||
        !request.contains("input") || !request["input"].is_object()) {
        out.push_back(makeEvent(context, "control.unsupported", requestId));
        return;
    }
    json payload = json::object();
    payload["request_id"] = requestId;
    payload["tool_name"] = request["tool_name"];
    payload["input"] = request["input"];
    if (request.contains("tool_use_id") && request["tool_use_id"].is_string())
        payload["tool_use_id"] = request["tool_use_id"];
    out.push_back(makeEvent(context, "permission.requested", payload.dump()));
}

} // namespace

std::vector<AgentEvent> parseClaudeJsonLine(std::string_view line, const RunContext &context) {
    std::vector<AgentEvent> events;
    json j;
    try {
        j = json::parse(line);
    } catch (...) {
        return events;
    }
    if (!j.is_object() || !j.contains("type") || !j["type"].is_string()) return events;
    try {
    const auto type = j["type"].get<std::string>();
    if (type == "assistant" && j.contains("parent_tool_use_id") && !j["parent_tool_use_id"].is_null())
        return events;
    if (type == "system") {
        if (j.value("subtype", "") == "init")
            if (auto sessionId = sessionEvent(j); !sessionId.empty())
                events.push_back(makeEvent(context, "run.session", sessionId));
    } else if (type == "assistant" && j.contains("message") && j["message"].is_object()) {
        handleAssistant(j["message"], context, events);
    } else if (type == "user" && j.contains("message") && j["message"].is_object()) {
        handleUser(j["message"], context, events);
    } else if (type == "result") {
        handleResult(j, context, events);
    } else if (type == "control_request") {
        handleControlRequest(j, context, events);
    }
    } catch (const json::exception &) {
        // Malformed typed fields are skipped rather than thrown.
    }
    return events;
}

} // namespace aegis::daemon::agents
