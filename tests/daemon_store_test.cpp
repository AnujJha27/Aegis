#include "daemon/session/store.h"
#include "daemon/protocol/event_hub.h"

#include <cassert>
#include <chrono>
#include <filesystem>
#include <string>

#include <sqlite3.h>

int main() {
    const auto path = std::filesystem::temp_directory_path() / "aegis-daemon-store-test.sqlite";
    const auto legacyPath = std::filesystem::temp_directory_path() / "aegis-daemon-store-legacy-test.sqlite";
    std::filesystem::remove(path);
    std::filesystem::remove(legacyPath);

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

        assert(store.tasks().size() == 1);
        assert(store.events(task.id).size() == 2);
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
        "INSERT INTO runs VALUES ('legacy-run','legacy-task','codex','starting',2,0);",
        nullptr, nullptr, nullptr) == SQLITE_OK);
    sqlite3_close(legacy);
    {
        aegis::daemon::Store migrated(legacyPath);
        const auto migratedRun = migrated.run("legacy-run");
        assert(migratedRun && migratedRun->status == "interrupted");
        assert(migratedRun->finishedAt > 0);
        assert(migrated.verifications("legacy-task").empty());
    }

    std::filesystem::remove(path);
    std::filesystem::remove(legacyPath);

    aegis::daemon::EventHub hub;
    const auto subscription = hub.subscribe();
    hub.publish({"event-3", "task", "run", "agent.finished", "shell", "ok", 3});
    aegis::daemon::AgentEvent event;
    assert(hub.wait(subscription, event, std::chrono::milliseconds(10)));
    assert(event.type == "agent.finished");
    assert(!hub.wait(subscription, event, std::chrono::milliseconds(10)));
    hub.unsubscribe(subscription);
    assert(!hub.wait(subscription, event, std::chrono::milliseconds(1)));
}
