#include "daemon/session/store.h"
#include "daemon/protocol/event_hub.h"

#include <cassert>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>

#include <sqlite3.h>

#include <algorithm>

int main() {
    const auto fixtureDirectory = std::filesystem::temp_directory_path() /
        ("aegis-daemon-store-test-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    assert(std::filesystem::create_directory(fixtureDirectory));
    const auto path = fixtureDirectory / "store.sqlite";
    const auto legacyPath = fixtureDirectory / "legacy.sqlite";
    const auto corruptPath = fixtureDirectory / "corrupt.sqlite";

    {
        aegis::daemon::Store store(path);
        const auto task = store.createTask("inspect the parser", "/tmp/repo");
        assert(!task.id.empty());
        assert(task.prompt == "inspect the parser");

        const auto run = store.startRun(task.id, "codex");
        assert(!run.id.empty());
        assert(run.taskId == task.id);
        assert(run.status == "starting");
        assert(store.updateRunStatus(run.id, "running"));
        assert(store.setExternalSessionId(run.id, "thread-123"));
        const auto activeRun = store.run(run.id);
        assert(activeRun && activeRun->status == "running");
        assert(activeRun->externalSessionId == "thread-123");
        assert(activeRun->finishedAt == 0);

        aegis::daemon::VerificationRun verification{
            "verification-1", task.id, run.id, {"cmake", "--build", "build"}, 0, "ok", 10, 20};
        store.saveVerification(verification);
        const auto history = store.verifications(task.id);
        assert(history.size() == 1);
        assert(history.front().command == verification.command);
        assert(history.front().runId == run.id);

        const auto finding = store.createFinding(task.id, run.id, "src/main.cpp", 7, 9, "Check cleanup on early return.");
        assert(finding.status == "open" && finding.startLine == 7 && finding.endLine == 9);
        assert(store.findings(task.id).size() == 1);
        assert(store.updateFindingStatus(finding.id, "resolved"));
        assert(!store.updateFindingStatus(finding.id, "critical"));

        assert(store.updateRunStatus(run.id, "completed"));
        for (const auto *state : {"failed", "interrupted", "terminated"}) {
            const auto terminalRun = store.startRun(task.id, "codex");
            assert(store.updateRunStatus(terminalRun.id, "running"));
            assert(store.updateRunStatus(terminalRun.id, state));
            assert(store.run(terminalRun.id)->finishedAt > 0);
        }
        const auto abandoned = store.startRun(task.id, "claude");
        assert(store.updateRunStatus(abandoned.id, "running"));

        store.appendEvent({"event-1", task.id, run.id, "agent.started", "codex", "", 1});
        store.appendEvent({"event-2", task.id, run.id, "agent.message.completed", "codex", "done", 2});
        for (int index = 3; index <= 27; ++index)
            store.appendEvent({"event-" + std::to_string(index), task.id, run.id, "agent.message.completed", "codex", "event", index});

        assert(store.tasks().size() == 1);
        assert(store.events(task.id).size() == 27);
        const auto recent = store.events(task.id, 3);
        assert(recent.size() == 3 && recent.front().id == "event-25" && recent.back().id == "event-27");
        const auto firstPage = store.eventsBefore(task.id, recent.front().sequence, 2);
        assert(firstPage.events.size() == 2 && firstPage.events.front().id == "event-23");
        assert(firstPage.hasMore && firstPage.nextCursor == firstPage.events.front().sequence);
        const auto secondPage = store.eventsBefore(task.id, firstPage.nextCursor, 2);
        assert(secondPage.events.size() == 2 && secondPage.events.front().id == "event-21");
        assert(store.events(task.id, 0).empty());
        const auto terminalRun = store.startRun(task.id, "shell");
        for (int index = 0; index < 140; ++index)
            store.appendEvent({"terminal-" + std::to_string(index), task.id, terminalRun.id, "terminal.output", "shell", "chunk", index});
        store.appendEvent({"terminal-semantic", task.id, terminalRun.id, "run.completed", "shell", "", 141});
        const auto terminalHistory = store.events(task.id);
        const auto retainedTerminalChunks = std::count_if(terminalHistory.begin(), terminalHistory.end(), [&](const auto &item) {
            return item.runId == terminalRun.id && item.type == "terminal.output";
        });
        assert(retainedTerminalChunks == 128);
        assert(store.terminalOutput(terminalRun.id).size() == 128);
        assert(std::any_of(terminalHistory.begin(), terminalHistory.end(), [&](const auto &item) {
            return item.runId == terminalRun.id && item.type == "run.completed";
        }));
    }

    {
        aegis::daemon::Store store(path);
        assert(store.tasks().size() == 1);
        assert(store.events(store.tasks().front().id).front().content == "");
        const auto run = store.runs(store.tasks().front().id).front();
        assert(run.status == "completed");
        const auto finished = store.run(run.id);
        assert(finished && finished->status == "completed");
        assert(finished->finishedAt > 0);
        assert(!store.updateRunStatus(run.id, "running"));
        assert(store.run("run-absent") == std::nullopt);
        const auto findings = store.findings(store.tasks().front().id);
        assert(findings.size() == 1 && findings.front().status == "resolved");
        const auto abandoned = store.runs(store.tasks().front().id).back();
        assert(abandoned.status == "interrupted");
        assert(abandoned.finishedAt > 0);
    }

    sqlite3 *legacy = nullptr;
    assert(sqlite3_open(legacyPath.string().c_str(), &legacy) == SQLITE_OK);
    assert(sqlite3_exec(legacy,
        "CREATE TABLE tasks (id TEXT PRIMARY KEY, prompt TEXT NOT NULL, repository TEXT NOT NULL, status TEXT NOT NULL, created_at INTEGER NOT NULL);"
        "CREATE TABLE runs (id TEXT PRIMARY KEY, task_id TEXT NOT NULL REFERENCES tasks(id), agent TEXT NOT NULL, status TEXT NOT NULL, started_at INTEGER NOT NULL, finished_at INTEGER NOT NULL DEFAULT 0);"
        "CREATE TABLE events (id TEXT PRIMARY KEY, task_id TEXT NOT NULL REFERENCES tasks(id), run_id TEXT NOT NULL, type TEXT NOT NULL, agent TEXT NOT NULL, content TEXT NOT NULL, timestamp INTEGER NOT NULL);"
        "INSERT INTO tasks VALUES ('legacy-task','legacy','/tmp','open',1);"
        "INSERT INTO runs VALUES ('legacy-run','legacy-task','codex','starting',2,0);"
        "INSERT INTO events VALUES ('legacy-event','legacy-task','legacy-run','user.message','codex','preserved',3);",
        nullptr, nullptr, nullptr) == SQLITE_OK);
    assert(sqlite3_exec(legacy, "PRAGMA user_version = 1;", nullptr, nullptr, nullptr) == SQLITE_OK);
    sqlite3_close(legacy);
    {
        aegis::daemon::Store migrated(legacyPath);
        const auto migratedRun = migrated.run("legacy-run");
        assert(migratedRun && migratedRun->status == "interrupted");
        assert(migratedRun->finishedAt > 0);
        assert(migrated.events("legacy-task").size() == 1);
        assert(migrated.events("legacy-task").front().sequence > 0);
        assert(migrated.eventsBefore("legacy-task", 2, 10).events.front().id == "legacy-event");
        assert(migrated.verifications("legacy-task").empty());
        assert(migrated.findings("legacy-task").empty());
        const auto migratedFinding = migrated.createFinding("legacy-task", std::nullopt, "legacy.cpp", 1, std::nullopt, "Migration retained the task.");
        assert(migratedFinding.status == "open");
    }
    assert(sqlite3_open(legacyPath.string().c_str(), &legacy) == SQLITE_OK);
    sqlite3_stmt *version = nullptr;
    assert(sqlite3_prepare_v2(legacy, "PRAGMA user_version", -1, &version, nullptr) == SQLITE_OK);
    assert(sqlite3_step(version) == SQLITE_ROW && sqlite3_column_int(version, 0) == 4);
    sqlite3_finalize(version);
    sqlite3_stmt *eventIndex = nullptr;
    assert(sqlite3_prepare_v2(legacy, "SELECT 1 FROM sqlite_master WHERE type='index' AND name='events_task_sequence'", -1, &eventIndex, nullptr) == SQLITE_OK);
    assert(sqlite3_step(eventIndex) == SQLITE_ROW);
    sqlite3_finalize(eventIndex);
    sqlite3_close(legacy);

    const std::string corruptContents = "preserve this invalid database";
    {
        std::ofstream corrupt(corruptPath, std::ios::binary);
        corrupt << corruptContents;
    }
    bool reportedPath = false;
    try {
        aegis::daemon::Store corrupt(corruptPath);
    } catch (const std::runtime_error &error) {
        reportedPath = std::string(error.what()).find(corruptPath.string()) != std::string::npos;
    }
    assert(reportedPath);
    std::ifstream preserved(corruptPath, std::ios::binary);
    assert(std::string(std::istreambuf_iterator<char>(preserved), {}) == corruptContents);

    std::filesystem::remove_all(fixtureDirectory);

    aegis::daemon::EventHub hub;
    const auto subscription = hub.subscribe();
    hub.publish({"event-3", "task", "run", "agent.finished", "shell", "ok", 3});
    aegis::daemon::AgentEvent event;
    assert(hub.wait(subscription, event, std::chrono::milliseconds(10)));
    assert(event.type == "agent.finished");
    assert(!hub.wait(subscription, event, std::chrono::milliseconds(10)));
    hub.unsubscribe(subscription);
    assert(!hub.wait(subscription, event, std::chrono::milliseconds(1)));

    aegis::daemon::EventHub runHub;
    const auto runSubscription = runHub.subscribe("run-2");
    runHub.publish({"other", "task", "run-1", "terminal.output", "shell", "ignored", 1});
    runHub.publish({"wanted", "task", "run-2", "terminal.output", "shell", "kept", 2});
    assert(runHub.wait(runSubscription, event, std::chrono::milliseconds(10)) && event.id == "wanted");
    assert(!runHub.wait(runSubscription, event, std::chrono::milliseconds(1)));
    runHub.unsubscribe(runSubscription);

    aegis::daemon::EventHub boundedHub;
    const auto slow = boundedHub.subscribe();
    const auto fast = boundedHub.subscribe();
    for (std::size_t index = 0; index < aegis::daemon::EventHub::maxQueuedEvents + 1; ++index) {
        boundedHub.publish({"burst-" + std::to_string(index), "task", "run", "turn.started", "codex", "", static_cast<std::int64_t>(index)});
        aegis::daemon::AgentEvent delivered;
        assert(boundedHub.wait(fast, delivered, std::chrono::milliseconds(1)));
        assert(delivered.id == "burst-" + std::to_string(index));
    }
    std::size_t slowEvents = 0;
    bool sawResync = false;
    while (boundedHub.wait(slow, event, std::chrono::milliseconds(1))) {
        ++slowEvents;
        if (event.type == "stream.resync_required") {
            sawResync = true;
            break;
        }
    }
    assert(sawResync);
    assert(slowEvents <= aegis::daemon::EventHub::maxQueuedEvents);
    assert(!boundedHub.wait(slow, event, std::chrono::milliseconds(1)));
    boundedHub.unsubscribe(slow);
    boundedHub.unsubscribe(fast);

    aegis::daemon::EventHub byteBoundedHub;
    const auto byteSlow = byteBoundedHub.subscribe();
    const std::string largeEvent(1024 * 1024, 'x');
    for (int index = 0; index < 5; ++index)
        byteBoundedHub.publish({"large-" + std::to_string(index), "task", "run", "user.message", "codex", largeEvent, index});
    sawResync = false;
    while (byteBoundedHub.wait(byteSlow, event, std::chrono::milliseconds(1))) {
        if (event.type == "stream.resync_required") { sawResync = true; break; }
    }
    assert(sawResync);
    byteBoundedHub.unsubscribe(byteSlow);
}
