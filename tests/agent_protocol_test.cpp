#include "daemon/agents/claude_protocol.h"
#include "daemon/agents/codex_adapter.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cassert>
#include <string>
#include <vector>

using namespace aegis::daemon::agents;
using aegis::daemon::AgentEvent;
using json = nlohmann::json;

namespace {
RunContext ctx() { return {"task1", "run1", "claude", {}, std::nullopt}; }
const AgentEvent *find(const std::vector<AgentEvent> &events, const std::string &type) {
    auto it = std::find_if(events.begin(), events.end(),
                           [&](const AgentEvent &e) { return e.type == type; });
    return it == events.end() ? nullptr : &*it;
}
} // namespace

int main() {
    const auto context = ctx();

    // system init session
    auto events = parseClaudeJsonLine(
        R"({"type":"system","subtype":"init","session_id":"sess-1"})", context);
    assert(events.size() == 1 && events[0].type == "run.session" && events[0].content == "sess-1");

    // assistant: tool_use may come before text; find by type, not order
    events = parseClaudeJsonLine(
        R"({"type":"assistant","message":{"content":[{"type":"tool_use","id":"tu1","name":"Bash","input":{"cmd":"ls"}},{"type":"text","text":"hi"}]}})",
        context);
    const auto *started = find(events, "tool.started");
    assert(started);
    {
        json p = json::parse(started->content);
        assert(p["id"] == "tu1" && p["name"] == "Bash" && p["input"]["cmd"] == "ls");
    }
    const auto *msg = find(events, "agent.message.completed");
    assert(msg && msg->content == "hi");

    // user tool_result
    events = parseClaudeJsonLine(
        R"({"type":"user","message":{"content":[{"type":"tool_result","tool_use_id":"tu1","content":"ok","is_error":false}]}})",
        context);
    const auto *done = find(events, "tool.completed");
    assert(done);
    {
        json p = json::parse(done->content);
        assert(p["tool_use_id"] == "tu1" && p["is_error"] == false);
    }

    // permission request: exact request_id/input
    events = parseClaudeJsonLine(
        R"({"type":"control_request","request_id":"r9","request":{"subtype":"can_use_tool","tool_name":"Bash","input":{"cmd":"rm"}}})",
        context);
    const auto *perm = find(events, "permission.requested");
    assert(perm);
    {
        json p = json::parse(perm->content);
        assert(p["request_id"] == "r9" && p["tool_name"] == "Bash" && p["input"]["cmd"] == "rm");
    }

    // malformed shape must not throw
    (void)parseClaudeJsonLine(R"({"type":"assistant","message":{"content":"nope"}})", context);
    (void)parseClaudeJsonLine("not json", context);

    // subagent events ignored
    events = parseClaudeJsonLine(
        R"({"type":"assistant","parent_tool_use_id":"tu1","message":{"content":[{"type":"text","text":"sub"}]}})",
        context);
    assert(events.empty());

    // result error -> run.failed
    events = parseClaudeJsonLine(
        R"({"type":"result","is_error":true,"errors":["boom"]})", context);
    const auto *failed = find(events, "run.failed");
    assert(failed && failed->content == "boom");

    // Codex: item.completed tool items -> tool.completed with id/name/status/details
    const std::string codexAgent = "codex";
    const std::string commandLine =
        R"({"type":"item.completed","item":{"type":"command_execution","id":"c1","name":"make","status":"failed","command":"make test","exit_code":3,"aggregated_output":"error: fail"}})";
    auto codex = parseCodexJsonOutput(commandLine, "t", "r", codexAgent);
    const auto *tool = find(codex, "tool.completed");
    assert(tool);
    {
        json p = json::parse(tool->content);
        assert(p["id"] == "c1" && p["name"] == "command_execution" && p["status"] == "failed");
        const json details = json::parse(p["details"].get<std::string>());
        assert(details.contains("exit_code") && details["exit_code"] == 3);
        const std::string out = details["aggregated_output"].get<std::string>();
        assert(out.find("error: fail") != std::string::npos);
    }
    for (const std::string itemType : {"file_change", "mcp_tool_call", "web_search"}) {
        const std::string line = json{{"type", "item.completed"},
                                      {"item", json{{"type", itemType}, {"id", "x1"},
                                                     {"name", itemType}, {"status", "completed"}}}}
                                   .dump();
        auto ev = parseCodexJsonOutput(line, "t", "r", codexAgent);
        assert(find(ev, "tool.completed"));
        json p = json::parse(find(ev, "tool.completed")->content);
        assert(p["id"] == "x1" && p["name"] == itemType && p["status"] == "completed");
        assert(p.contains("details"));
    }

    // reasoning ignored
    codex = parseCodexJsonOutput(
        R"({"type":"item.completed","item":{"type":"reasoning","text":"thinking"}})", "t", "r", codexAgent);
    assert(codex.empty());

    // Codex error -> run.failed
    codex = parseCodexJsonOutput(R"({"type":"error","message":"nope"})", "t", "r", codexAgent);
    assert(find(codex, "run.failed"));

    return 0;
}
