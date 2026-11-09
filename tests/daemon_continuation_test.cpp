#include "daemon/api/routes.h"
#include "daemon/agents/manager.h"
#include "daemon/repository/files.h"
#include "daemon/repository/git.h"

#include "repository_fixture.h"

#include <boost/beast/http.hpp>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <string>
#include <thread>
#include <vector>

using namespace aegis::daemon;
namespace http = boost::beast::http;
namespace fs = std::filesystem;
using Json = nlohmann::json;

namespace {

http::request<http::string_body> req(http::verb method, const std::string &target, const Json &body) {
    http::request<http::string_body> r{method, target, 11};
    r.set(http::field::content_type, "application/json");
    r.body() = body.dump();
    r.prepare_payload();
    return r;
}

Json contextBody(const Json &goal) {
    return {{"goal", goal}, {"constraints", "Keep API"}, {"decisions", "Use current parser"},
        {"attempts", "Regex failed"}, {"unfinished", "Tests"}, {"next_action", "Add regression"}};
}

struct EnvGuard {
    std::string name;
    std::string old;
    EnvGuard(std::string n) : name(std::move(n)) {
        const char *v = std::getenv(name.c_str());
        old = v ? v : "";
    }
    ~EnvGuard() {
        if (old.empty()) unsetenv(name.c_str());
        else setenv(name.c_str(), old.c_str(), 1);
    }
};

bool waitUntil(const std::function<bool()> &pred) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(5010);
    while (std::chrono::steady_clock::now() < deadline) {
        if (pred()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return pred();
}

std::vector<std::string> logTokens(const fs::path &p) {
    std::ifstream in(p, std::ios::binary);
    const std::string data{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
    std::vector<std::string> out;
    std::string cur;
    for (const char c : data) {
        if (c == '\0') { out.push_back(cur); cur.clear(); }
        else cur.push_back(c);
    }
    if (!cur.empty()) out.push_back(cur);
    return out;
}

bool waitLogPrompt(const fs::path &p, const std::string &prompt) {
    return waitUntil([&] {
        const auto tokens = logTokens(p);
        return std::find(tokens.begin(), tokens.end(), prompt) != tokens.end();
    });
}

bool waitLogQuiet(const fs::path &p) {
    auto size = [&] {
        std::error_code ec;
        const auto s = fs::file_size(p, ec);
        return ec ? std::uintmax_t{0} : s;
    };
    std::uintmax_t last = size();
    int stable = 0;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(5010);
    while (std::chrono::steady_clock::now() < deadline) {
        const auto s = size();
        if (s == last) { if (++stable >= 20) return true; }
        else { last = s; stable = 0; }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return true;
}

bool waitEvent(const Store &store, const std::string &taskId, const std::string &runId, const std::string &type) {
    return waitUntil([&] {
        for (const auto &e : store.events(taskId))
            if (e.runId == runId && e.type == type) return true;
        return false;
    });
}

std::size_t countRunEvents(const Store &store, const std::string &taskId, const std::string &runId) {
    std::size_t n = 0;
    for (const auto &e : store.events(taskId))
        if (e.runId == runId) ++n;
    return n;
}

void assertOneUserMessage(const Store &store, const std::string &taskId, const std::string &runId, const std::string &payload) {
    const bool ok = waitUntil([&] {
        std::size_t n = 0;
        for (const auto &e : store.events(taskId))
            if (e.runId == runId && e.type == "user.message" && e.content == payload) ++n;
        return n == 1;
    });
    assert(ok);
}

void writeScript(const fs::path &p, const std::string &body) {
    std::ofstream f(p);
    f << body;
    f.close();
    fs::permissions(p, fs::perms::owner_exec | fs::perms::owner_read);
}

} // namespace

int main() {
    RepositoryFixture fixture;
    Store store{":memory:"};
    EventHub events;
    repository::GitRepository gitRepo{fixture.root()};
    repository::Files files{gitRepo};
    agents::Manager manager{fixture.root(), store, events};
    api::Context context;
    context.store = &store;
    context.events = &events;
    context.agentManager = &manager;
    context.git = &gitRepo;
    context.files = &files;
    context.repository = fixture.root();

    EnvGuard pathGuard{"PATH"};
    EnvGuard logGuard{"FAKE_LOG"};
    EnvGuard delayGuard{"FAKE_CODEX_DELAY"};

    const fs::path fakeBin = fixture.root() / "fakebin";
    fs::create_directories(fakeBin);
    const std::string logArgs =
        "for a in \"$@\"; do printf '%s\\0' \"$a\" >> \"$FAKE_LOG\"; done\n";
    writeScript(fakeBin / "codex",
                "#!/bin/sh\n" + logArgs +
                    "if [ -n \"$FAKE_CODEX_DELAY\" ]; then sleep \"$FAKE_CODEX_DELAY\"; fi\n"
                    "printf '%s\n' '{\"type\":\"thread.started\",\"thread_id\":\"fake-thread\"}'\n"
                    "printf '%s\n' '{\"type\":\"item.completed\",\"item\":{\"type\":\"agent_message\",\"text\":\"ok\"}}'\n");
    writeScript(fakeBin / "claude", "#!/bin/sh\n" + logArgs);
    writeScript(fakeBin / "opencode", "#!/bin/sh\n" + logArgs);
    setenv("PATH", (fakeBin.string() + ":" + pathGuard.old).c_str(), 1);

    { std::ofstream f(fixture.root() / "source.cpp"); f << "int main() { return 0; }\n"; }
    fixture.commit("seed source.cpp");

    const Json full = contextBody("Fix parser");
    const auto task = store.createTask("Fix the parser", fixture.root().string());
    const auto sourceRun = store.startRun(task.id, "codex");
    assert(store.updateRunStatus(sourceRun.id, "completed"));
    store.appendEvent(AgentEvent{"seed-event-0001", task.id, sourceRun.id, "turn.completed", "codex", "seed", 1700000000000LL});
    const auto sourceStatusBefore = store.run(sourceRun.id)->status;
    const auto sourceEventsBefore = countRunEvents(store, task.id, sourceRun.id);

    auto res = api::handle(req(http::verb::patch, "/api/tasks/" + task.id + "/context", full), context);
    assert(res.result() == http::status::ok);

    res = api::handle(req(http::verb::get, "/api/tasks", Json::object()), context);
    assert(res.result() == http::status::ok);
    const Json listed = Json::parse(res.body());
    const Json *tasks = listed.is_array() ? &listed : &listed.at("tasks");
    bool found = false;
    for (const auto &entry : *tasks) {
        if (entry.value("id", std::string{}) == task.id) {
            found = true;
            assert(entry.at("context") == full);
        }
    }
    assert(found);

    res = api::handle(req(http::verb::get, "/api/tasks/" + task.id + "/handoff", Json::object()), context);
    assert(res.result() == http::status::ok);
    assert(Json::parse(res.body()).at("context") == full);

    for (const auto &goal : {Json(5), Json(std::string("bad\0value", 9)), Json(std::string(4001, 'x'))}) {
        res = api::handle(req(http::verb::patch, "/api/tasks/" + task.id + "/context", contextBody(goal)), context);
        assert(res.result() == http::status::bad_request);
        res = api::handle(req(http::verb::get, "/api/tasks/" + task.id + "/handoff", Json::object()), context);
        assert(Json::parse(res.body()).at("context") == full);
    }

    res = api::handle(req(http::verb::patch, "/api/tasks/missing/context", full), context);
    assert(res.result() == http::status::not_found);

    const auto fresh = store.createTask("Another task", fixture.root().string());
    res = api::handle(req(http::verb::get, "/api/tasks", Json::object()), context);
    const Json freshList = Json::parse(res.body());
    const Json *ftasks = freshList.is_array() ? &freshList : &freshList.at("tasks");
    for (const auto &entry : *ftasks) {
        if (entry.value("id", std::string{}) == fresh.id) {
            const Json &c = entry.at("context");
            for (const char *k : {"goal", "constraints", "decisions", "attempts", "unfinished", "next_action"})
                assert(c.value(k, std::string{}) == "");
        }
    }

    auto deliver = [&](const std::string &taskId, const Json &body) {
        return api::handle(req(http::verb::post, "/api/tasks/" + taskId + "/continue", body), context);
    };

    const std::string payload =
        std::string("-continue the work\nwith \"quotes\" and $(touch sentinel)");
    const fs::path sentinel = fixture.root() / "sentinel";
    std::vector<std::string> launched{sourceRun.id};
    const fs::path negLog = fakeBin / "negative.log";

    for (const char *agent : {"claude", "opencode", "codex"}) {
        const std::string agentName = agent;
        const fs::path logPath = fakeBin / (agentName + ".log");
        setenv("FAKE_LOG", logPath.string().c_str(), 1);
        const auto created = deliver(task.id, Json{{"agent", agentName}, {"prompt", payload}, {"source_run_id", sourceRun.id}});
        assert(created.result() == http::status::created);
        const std::string runId = Json::parse(created.body()).at("id").get<std::string>();
        assert(!runId.empty());
        const auto run = store.run(runId);
        assert(run.has_value() && run->taskId == task.id && run->agent == agentName);
        launched.push_back(runId);
        assertOneUserMessage(store, task.id, runId, payload);
        assert(waitLogPrompt(logPath, payload));
        const auto tokens = logTokens(logPath);
        if (agentName == "codex") {
            assert(waitEvent(store, task.id, runId, "turn.completed"));
            assert(tokens.size() >= 2);
            assert(tokens[tokens.size() - 2] == "--");
            assert(tokens.back() == payload);
            manager.terminate(runId);
            waitLogQuiet(logPath);
        } else if (agentName == "claude") {
            const std::vector<std::string> expected{"--", payload};
            assert(tokens == expected);
        } else {
            const std::vector<std::string> expected{"--prompt", payload};
            assert(tokens == expected);
        }
        assert(!fs::exists(sentinel));
    }

    std::size_t continued = 0;
    for (const auto &e : store.events(task.id))
        if (e.type == "run.continued" && e.content == sourceRun.id && e.taskId == task.id && !e.id.empty())
            ++continued;
    assert(continued >= 3);

    const auto failedRun = store.startRun(task.id, "codex");
    assert(store.updateRunStatus(failedRun.id, "failed"));
    store.appendEvent(AgentEvent{"seed-event-0002", task.id, failedRun.id, "turn.started", "codex", "", 1700000000001LL});
    {
        const fs::path logPath = fakeBin / "resume-failed.log";
        setenv("FAKE_LOG", logPath.string().c_str(), 1);
        const auto created = deliver(task.id, Json{{"agent", "codex"}, {"prompt", payload}, {"source_run_id", failedRun.id}});
        assert(created.result() == http::status::created);
        const std::string runId = Json::parse(created.body()).at("id").get<std::string>();
        launched.push_back(runId);
        assert(waitLogPrompt(logPath, payload));
        assert(waitEvent(store, task.id, runId, "turn.completed"));
        manager.terminate(runId);
        waitLogQuiet(logPath);
    }

    const std::vector<Json> badBodies = {
        Json{{"agent", "codex"}, {"prompt", std::string(65537, 'x')}, {"source_run_id", sourceRun.id}},
        Json{{"agent", "codex"}, {"prompt", std::string{}}, {"source_run_id", sourceRun.id}},
        Json{{"agent", "codex"}, {"prompt", std::string("   ")}, {"source_run_id", sourceRun.id}},
        Json{{"agent", "codex"}, {"prompt", std::string("a\0b", 3)}, {"source_run_id", sourceRun.id}},
        Json{{"agent", "shell"}, {"prompt", "-x"}, {"source_run_id", sourceRun.id}},
        Json{{"agent", "unknown-agent"}, {"prompt", "-x"}, {"source_run_id", sourceRun.id}},
    };
    setenv("FAKE_LOG", negLog.string().c_str(), 1);
    for (const auto &body : badBodies)
        assert(deliver(task.id, body).result() == http::status::bad_request);
    assert(deliver(fresh.id, Json{{"agent", "codex"}, {"prompt", payload}, {"source_run_id", sourceRun.id}})
               .result() == http::status::not_found);
    assert(store.runs(fresh.id).empty());
    assert(!fs::exists(sentinel));

    setenv("FAKE_CODEX_DELAY", "3", 1);
    std::string activeRunId;
    {
        const fs::path logPath = fakeBin / "active.log";
        setenv("FAKE_LOG", logPath.string().c_str(), 1);
        const auto created = deliver(task.id, Json{{"agent", "codex"}, {"prompt", payload}, {"source_run_id", sourceRun.id}});
        assert(created.result() == http::status::created);
        activeRunId = Json::parse(created.body()).at("id").get<std::string>();
        launched.push_back(activeRunId);
        assertOneUserMessage(store, task.id, activeRunId, payload);
        assert(waitEvent(store, task.id, activeRunId, "turn.started"));
        const auto message = api::handle(
            req(http::verb::post, "/api/runs/" + activeRunId + "/messages", Json{{"message", "-again"}}), context);
        assert(message.result() == http::status::conflict);
        const auto busy = deliver(task.id, Json{{"agent", "codex"}, {"prompt", "-busy"}, {"source_run_id", activeRunId}});
        assert(busy.result() == http::status::conflict);
        unsetenv("FAKE_CODEX_DELAY");
        manager.terminate(activeRunId);
        waitLogQuiet(logPath);
    }

    for (const auto &id : launched) manager.terminate(id);
    for (const char *n : {"codex", "resume-failed", "active"}) waitLogQuiet(fakeBin / (std::string(n) + ".log"));

    for (const auto &e : store.events(task.id)) {
        assert(!e.id.empty());
        assert(e.taskId == task.id);
        assert(!e.type.empty() && !e.agent.empty());
    }
    assert(store.runs(task.id).size() == launched.size() + 1);
    const auto sourceAfter = store.run(sourceRun.id);
    assert(sourceAfter.has_value() && sourceAfter->status == sourceStatusBefore);
    assert(countRunEvents(store, task.id, sourceRun.id) == sourceEventsBefore);
    assert(!fs::exists(sentinel));

    return 0;
}
