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

#include <nlohmann/json.hpp>

int main() {
    const auto fixtureDirectory = std::filesystem::temp_directory_path() /
        ("aegis-daemon-store-test-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    assert(std::filesystem::create_directory(fixtureDirectory));
    const auto path = fixtureDirectory / "store.sqlite";
    const auto legacyPath = fixtureDirectory / "legacy.sqlite";
    const auto corruptPath = fixtureDirectory / "corrupt.sqlite";
    const auto legacyV4Path = fixtureDirectory / "legacy-v4.sqlite";

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
        verification.repositoryFingerprint = "fingerprint-abc";
        verification.freshness = "current";
        store.saveVerification(verification);
        const auto history = store.verifications(task.id);
        assert(history.size() == 1);
        assert(history.front().command == verification.command);
        assert(history.front().runId == run.id);
        assert(history.front().repositoryFingerprint == "fingerprint-abc");
        assert(history.front().freshness == "unknown");

        const auto finding = store.createFinding(task.id, run.id, "src/main.cpp", 7, 9, "Check cleanup on early return.");
        assert(finding.status == "open" && finding.startLine == 7 && finding.endLine == 9);
        assert(store.findings(task.id).size() == 1);
        const auto fingerprinted = store.createFinding(task.id, std::nullopt, "src/util.cpp", std::nullopt, std::nullopt,
            "Fingerprinted finding.", std::optional<std::string>("fp-123"));
        assert(fingerprinted.repositoryFingerprint == "fp-123" && fingerprinted.freshness == "unknown");
        assert(store.updateFindingStatus(finding.id, "resolved"));
        assert(!store.updateFindingStatus(finding.id, "critical"));
        const auto ordered = store.findings(task.id);
        assert(ordered.size() == 2);
        assert(ordered.front().id == fingerprinted.id && ordered.front().status == "open");
        assert(ordered.back().id == finding.id && ordered.back().status == "resolved");

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
        aegis::daemon::Store store(":memory:");
        const auto task = store.createTask("interleaved terminal retention", "/repo");
        const auto targetRun = store.startRun(task.id, "shell");
        const auto otherRun = store.startRun(task.id, "shell");
        const auto align = std::size_t{1};
        for (std::size_t filler = 0; filler < align; ++filler)
            store.appendEvent({"align-" + std::to_string(filler), task.id, otherRun.id, "terminal.output", "shell", "chunk", 0});
        for (int index = 0; index < 200; ++index) {
            for (int filler = 0; filler < 31; ++filler)
                store.appendEvent({"other-" + std::to_string(index * 31 + filler), task.id, otherRun.id, "terminal.output", "shell", "chunk", index});
            store.appendEvent({"target-" + std::to_string(index), task.id, targetRun.id, "terminal.output", "shell", "chunk", index});
        }
        const auto retainedWhileActive = store.events(task.id);
        const auto countTerminal = [&](const std::string &runId) {
            return std::count_if(retainedWhileActive.begin(), retainedWhileActive.end(), [&](const auto &event) {
                return event.runId == runId && event.type == "terminal.output";
            });
        };
        assert(countTerminal(targetRun.id) >= 128 && countTerminal(targetRun.id) < 160);
        assert(countTerminal(otherRun.id) >= 128 && countTerminal(otherRun.id) < 160);
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
        assert(findings.size() == 2);
        assert(findings.front().status == "open" && findings.front().repositoryFingerprint == "fp-123");
        assert(findings.front().freshness == "unknown");
        assert(findings.back().status == "resolved");
        const auto abandoned = store.runs(store.tasks().front().id).back();
        assert(abandoned.status == "interrupted");
        assert(abandoned.finishedAt > 0);
        const auto reopenedVerifications = store.verifications(store.tasks().front().id);
        assert(reopenedVerifications.size() == 1);
        assert(reopenedVerifications.front().repositoryFingerprint == "fingerprint-abc");
        assert(reopenedVerifications.front().freshness == "unknown");
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
        const auto migratedTask = migrated.task("legacy-task");
        assert(migratedTask && migratedTask->context.goal.empty() && migratedTask->context.nextAction.empty());
        const aegis::daemon::TaskContext migratedContext{"goal-x", "constraints-x", "decisions-x", "attempts-x", "unfinished-x", "next-x"};
        assert(migrated.updateTaskContext("legacy-task", migratedContext));
        assert(!migrated.updateTaskContext("absent-task", migratedContext));
        bool rejected = false;
        try {
            aegis::daemon::TaskContext oversized = migratedContext;
            oversized.goal.assign(4001, 'x');
            migrated.updateTaskContext("legacy-task", oversized);
        } catch (const std::invalid_argument &) { rejected = true; }
        assert(rejected);
        const auto contextTask = migrated.task("legacy-task");
        assert(contextTask && contextTask->context.goal == migratedContext.goal &&
               contextTask->context.constraints == migratedContext.constraints &&
               contextTask->context.decisions == migratedContext.decisions &&
               contextTask->context.attempts == migratedContext.attempts &&
               contextTask->context.unfinished == migratedContext.unfinished &&
               contextTask->context.nextAction == migratedContext.nextAction);
        const auto migratedFinding = migrated.createFinding("legacy-task", std::nullopt, "legacy.cpp", 1, std::nullopt, "Migration retained the task.");
        assert(migratedFinding.status == "open");
        assert(!migratedFinding.repositoryFingerprint.has_value());
        assert(migratedFinding.freshness == "unknown");
        const aegis::daemon::VerificationRun legacyVerification{
            "legacy-verification", "legacy-task", std::nullopt, {"true"}, 0, "", 4, 4};
        migrated.saveVerification(legacyVerification);
        const auto legacyVerifications = migrated.verifications("legacy-task");
        assert(legacyVerifications.size() == 1);
        assert(!legacyVerifications.front().repositoryFingerprint.has_value());
        assert(legacyVerifications.front().freshness == "unknown");
    }
    assert(sqlite3_open(legacyPath.string().c_str(), &legacy) == SQLITE_OK);
    sqlite3_stmt *version = nullptr;
    assert(sqlite3_prepare_v2(legacy, "PRAGMA user_version", -1, &version, nullptr) == SQLITE_OK);
    assert(sqlite3_step(version) == SQLITE_ROW && sqlite3_column_int(version, 0) == aegis::daemon::Store::currentSchemaVersion);
    sqlite3_finalize(version);
    sqlite3_stmt *eventIndex = nullptr;
    assert(sqlite3_prepare_v2(legacy, "SELECT 1 FROM sqlite_master WHERE type='index' AND name='events_task_sequence'", -1, &eventIndex, nullptr) == SQLITE_OK);
    assert(sqlite3_step(eventIndex) == SQLITE_ROW);
    sqlite3_finalize(eventIndex);
    sqlite3_close(legacy);

    sqlite3 *legacyV4 = nullptr;
    assert(sqlite3_open(legacyV4Path.string().c_str(), &legacyV4) == SQLITE_OK);
    assert(sqlite3_exec(legacyV4,
        "PRAGMA user_version = 4;"
        "CREATE TABLE tasks (id TEXT PRIMARY KEY, prompt TEXT NOT NULL, repository TEXT NOT NULL, status TEXT NOT NULL, created_at INTEGER NOT NULL);"
        "CREATE TABLE runs (id TEXT PRIMARY KEY, task_id TEXT NOT NULL REFERENCES tasks(id), agent TEXT NOT NULL, status TEXT NOT NULL, started_at INTEGER NOT NULL, finished_at INTEGER NOT NULL DEFAULT 0, external_session_id TEXT);"
        "CREATE TABLE events (sequence INTEGER PRIMARY KEY AUTOINCREMENT, id TEXT NOT NULL UNIQUE, task_id TEXT NOT NULL REFERENCES tasks(id), run_id TEXT NOT NULL, type TEXT NOT NULL, agent TEXT NOT NULL, content TEXT NOT NULL, timestamp INTEGER NOT NULL);"
        "CREATE TABLE verifications (id TEXT PRIMARY KEY, task_id TEXT NOT NULL REFERENCES tasks(id), run_id TEXT REFERENCES runs(id) ON DELETE SET NULL, command_json TEXT NOT NULL, exit_code INTEGER NOT NULL, output TEXT NOT NULL, started_at INTEGER NOT NULL, finished_at INTEGER NOT NULL);"
        "INSERT INTO tasks VALUES ('v4-task','v4','/tmp','open',1);"
        "INSERT INTO runs VALUES ('v4-run','v4-task','codex','completed',2,3,NULL);"
        "INSERT INTO events (id, task_id, run_id, type, agent, content, timestamp) VALUES ('v4-event','v4-task','v4-run','user.message','codex','preserved',4);"
        "INSERT INTO verifications VALUES ('v4-verification','v4-task','v4-run','[\"true\"]',0,'kept-output',4,4);",
        nullptr, nullptr, nullptr) == SQLITE_OK);
    sqlite3_close(legacyV4);
    {
        aegis::daemon::Store migrated(legacyV4Path);
        const auto v4Verifications = migrated.verifications("v4-task");
        assert(v4Verifications.size() == 1);
        assert(v4Verifications.front().output == "kept-output");
        assert(v4Verifications.front().command == std::vector<std::string>{"true"});
        assert(!v4Verifications.front().repositoryFingerprint.has_value());
        assert(migrated.events("v4-task").front().id == "v4-event");
    }

    const auto schema10Path = fixtureDirectory / "schema10.sqlite";
    sqlite3 *schema10 = nullptr;
    assert(sqlite3_open(schema10Path.string().c_str(), &schema10) == SQLITE_OK);
    assert(sqlite3_exec(schema10,
        "PRAGMA user_version = 10;"
        "CREATE TABLE tasks (id TEXT PRIMARY KEY, prompt TEXT NOT NULL, repository TEXT NOT NULL, status TEXT NOT NULL, created_at INTEGER NOT NULL, context_json TEXT NOT NULL DEFAULT '{}');"
        "INSERT INTO tasks VALUES ('s10-task','schema10','/tmp','open',1,'{\"goal\":\"orig-goal\",\"constraints\":\"orig-constraints\",\"decisions\":\"orig-decisions\",\"attempts\":\"orig-attempts\",\"unfinished\":\"orig-unfinished\",\"next_action\":\"orig-next\"}');"
        "CREATE TABLE events (sequence INTEGER PRIMARY KEY AUTOINCREMENT, id TEXT NOT NULL UNIQUE, task_id TEXT NOT NULL, run_id TEXT NOT NULL, type TEXT NOT NULL, agent TEXT NOT NULL, content TEXT NOT NULL, timestamp INTEGER NOT NULL);",
        nullptr, nullptr, nullptr) == SQLITE_OK);
    sqlite3_close(schema10);
    {
        aegis::daemon::Store store(schema10Path);
        const auto migratedTask = store.task("s10-task");
        assert(migratedTask && migratedTask->context.goal == "orig-goal" &&
               migratedTask->context.constraints == "orig-constraints" &&
               migratedTask->context.nextAction == "orig-next");
        assert(migratedTask->contextRevision == 0);
        const aegis::daemon::TaskContext s10Context{"g1", "c1", "d1", "a1", "u1", "n1"};
        const auto casOne = store.updateTaskContext("s10-task", s10Context, 0);
        assert(casOne && casOne->contextRevision == 1 && casOne->context.goal == "g1");
        assert(!store.updateTaskContext("s10-task", s10Context, 0));
        assert(store.task("s10-task")->context.goal == "g1");
        const auto legacyUpdate = store.updateTaskContext("s10-task", s10Context);
        assert(legacyUpdate && legacyUpdate->contextRevision == 2);
        const auto allTasks = store.tasks();
        assert(allTasks.size() == 1 && allTasks.front().contextRevision == 2);
    }
    {
        aegis::daemon::Store store(schema10Path);
        const auto reopened = store.task("s10-task");
        assert(reopened && reopened->contextRevision == 2 && reopened->context.goal == "g1");
    }

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

    {
        const auto continuationPath = fixtureDirectory / "continuation-restart.sqlite";
        {
            aegis::daemon::Store store(continuationPath);
            const auto task = store.createTask("continuation restart", "/repo");
            const auto firstRun = store.startRun(task.id, "codex");
            assert(store.reserveContinuation("restart-1", nlohmann::json{{"prompt", "p"}, {"task_id", task.id}}.dump()));
            assert(store.linkContinuation("restart-1", firstRun.id));
            assert(store.completeContinuation("restart-2", firstRun.id) == false);
            assert(store.reserveContinuation("restart-2", nlohmann::json{{"prompt", "p"}, {"task_id", task.id}}.dump()));
            assert(store.linkContinuation("restart-2", firstRun.id));
            assert(store.completeContinuation("restart-2", firstRun.id));
        }
        {
            aegis::daemon::Store store(continuationPath);
            assert(store.continuationState("restart-1") == "uncertain");
            assert(store.continuation("restart-1")->second.has_value());
            assert(store.continuationState("restart-2") == "delivered");
        }
    }

    {
        const auto workingPath = fixtureDirectory / "working-profile.sqlite";
        const auto profile = nlohmann::json{{"version", 1}, {"instructions", nlohmann::json::array()}, {"workflows", nlohmann::json::array()}}.dump();
        {
            aegis::daemon::Store store(workingPath);
            const auto initial = store.workingProfile("/repo-a");
            assert(initial.second == 0);
            const auto parsedInitial = nlohmann::json::parse(initial.first);
            assert(parsedInitial.at("version") == 1 && parsedInitial.at("instructions").is_array() &&
                   parsedInitial.at("workflows").is_array());
            const auto first = store.saveWorkingProfile("/repo-a", profile, 0);
            assert(first && *first == 1);
            assert(!store.saveWorkingProfile("/repo-a", profile, 0));
            const auto afterConflict = store.workingProfile("/repo-a");
            assert(afterConflict.second == 1 && afterConflict.first == initial.first);
            const auto second = store.saveWorkingProfile("/repo-a", profile, 1);
            assert(second && *second == 2);
            const auto independent = store.workingProfile("/repo-b");
            assert(independent.second == 0 && independent.first == initial.first);
            bool rejected = false;
            try {
                store.saveWorkingProfile("/repo-a", nlohmann::json{{"version", 2}, {"instructions", nlohmann::json::array()}, {"workflows", nlohmann::json::array()}}.dump(), 2);
            } catch (const std::invalid_argument &) { rejected = true; }
            assert(rejected);
        }
        {
            aegis::daemon::Store store(workingPath);
            const auto persisted = store.workingProfile("/repo-a");
            assert(persisted.second == 2);
            const auto bump = store.saveWorkingProfile("/repo-a", profile, 2);
            assert(bump && *bump == 3);
            assert(store.workingProfile("/repo-b").second == 0);
        }
    }

    std::filesystem::remove_all(fixtureDirectory);

    {
        aegis::daemon::Store store(":memory:");
        const aegis::daemon::TaskContext ctx{"g", "c", "d", "a", "u", "n"};
        const std::vector<aegis::daemon::ImportedMessage> messages{
            {"m1", "user", "hello", 1, "session.jsonl"}, {"m2", "assistant", "hi", 2, "session.jsonl"}};
        const auto task = store.importTask("key-a", "imported prompt", "/repo", ctx, messages);
        assert(!task.id.empty() && task.context.goal == "g");
        const auto importedRuns = store.runs(task.id);
        assert(importedRuns.size() == 1 && importedRuns.front().status == "completed" &&
               importedRuns.front().agent == "import" && importedRuns.front().finishedAt > 0);
        assert(store.verifications(task.id).empty());
        const auto importedEvents = store.events(task.id);
        std::size_t messageEvents = 0;
        for (const auto &event : importedEvents)
            if (event.type == "import.message") {
                const auto payload = nlohmann::json::parse(event.content);
                assert(payload.at("historical").get<bool>());
                assert(payload.at("source_id").get<std::string>() == "m1" ||
                       payload.at("source_id").get<std::string>() == "m2");
                ++messageEvents;
            }
        assert(messageEvents == 2);
        const aegis::daemon::TaskContext edited{"g2", "c", "d", "a", "u", "n"};
        const auto again = store.importTask("key-a", "other", "/repo", edited, messages);
        assert(again.id == task.id && again.context.goal == "g");
        assert(store.events(task.id).size() == importedEvents.size());
        assert(store.runs(task.id).size() == 1);
        assert(store.tasks().size() == 1);
        const auto branch = store.importTask("key-b", "branch import", "/repo", ctx, messages);
        assert(branch.id != task.id);
        assert(store.tasks().size() == 2);
    }

    {
        aegis::daemon::Store store(":memory:");
        const auto task = store.createTask("continuation delivery state", "/repo");
        assert(store.continuationState("req-missing") == std::nullopt);
        assert(store.reserveContinuation("req-1", nlohmann::json{{"prompt", "p"}, {"task_id", task.id}}.dump()));
        assert(store.continuationState("req-1") == "prepared");
        const auto run = store.startRun(task.id, "codex");
        assert(store.linkContinuation("req-1", run.id));
        assert(store.continuationState("req-1") == "launching");
        assert(store.completeContinuation("req-1", run.id));
        assert(store.continuationState("req-1") == "delivered");
        assert(store.reserveContinuation("req-2", "{}"));
        const auto otherRun = store.startRun(task.id, "codex");
        assert(!otherRun.id.empty());
        assert(store.failContinuation("req-2"));
        assert(store.continuationState("req-2") == "failed");
        assert(store.continuationState("req-1") == "delivered");
    }

    {
        aegis::daemon::Store source(":memory:");
        const auto task = source.createTask("bundle roundtrip", "/repo-src");
        const aegis::daemon::TaskContext ctx{"bg", "bc", "bd", "ba", "bu", "bn"};
        assert(source.updateTaskContext(task.id, ctx));
        const auto run = source.startRun(task.id, "codex");
        source.appendEvent({"bundle-user-1", task.id, run.id, "user.message", "codex", "hello bundle", 1});
        assert(source.updateRunStatus(run.id, "completed"));

        const auto bundleJson = source.exportTaskBundle(task.id);
        const auto parsed = nlohmann::json::parse(bundleJson);
        assert(parsed.at("format") == "aegis-task" && parsed.at("version") == 1);

        aegis::daemon::Store destination(":memory:");
        assert(destination.tasks().empty());
        const auto importedTask = destination.importTaskBundle(bundleJson, "/repo-dst");
        assert(!importedTask.id.empty() && importedTask.id != task.id);
        assert(importedTask.context.goal == "bg" && importedTask.context.constraints == "bc" &&
               importedTask.context.decisions == "bd" && importedTask.context.attempts == "ba" &&
               importedTask.context.unfinished == "bu" && importedTask.context.nextAction == "bn");
        assert(destination.verifications(importedTask.id).empty());
        const auto importedRuns = destination.runs(importedTask.id);
        assert(importedRuns.size() == 1 && importedRuns.front().status == "completed" &&
               importedRuns.front().agent == "import");
        const auto importedEvents = destination.events(importedTask.id);
        bool sawUserMessage = false;
        bool sawProvenance = false;
        for (const auto &item : importedEvents) {
            if (item.type == "import.message") {
                const auto payload = nlohmann::json::parse(item.content);
                if (payload.at("role") == "user" && payload.value("text", "") == "hello bundle")
                    sawUserMessage = true;
            }
            if (item.type == "import.source") sawProvenance = true;
        }
        assert(sawUserMessage && sawProvenance);

        const auto reimported = destination.importTaskBundle(bundleJson, "/repo-dst");
        assert(reimported.id == importedTask.id);
        assert(destination.events(importedTask.id).size() == importedEvents.size());
        assert(destination.runs(importedTask.id).size() == 1);
        assert(destination.tasks().size() == 1);

        const auto taskCountBefore = destination.tasks().size();
        bool rejected = false;
        auto malformed = nlohmann::json::parse(bundleJson);
        malformed["version"] = 2;
        try {
            destination.importTaskBundle(malformed.dump(), "/repo-dst");
        } catch (const std::invalid_argument &) { rejected = true; }
        assert(rejected);
        assert(destination.tasks().size() == taskCountBefore);

        const auto importedProfile = destination.workingProfile("/repo-dst");
        assert(importedProfile.second == 0);
        const auto parsedProfile = nlohmann::json::parse(importedProfile.first);
        assert(parsedProfile.at("version") == 1 && parsedProfile.at("instructions").is_array());
    }

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
    const auto eventHighWater = boundedHub.highWater(slow);
    assert(eventHighWater.events == aegis::daemon::EventHub::maxQueuedEvents);
    assert(eventHighWater.bytes <= aegis::daemon::EventHub::maxQueuedBytes);
    assert(!boundedHub.wait(slow, event, std::chrono::milliseconds(1)));
    boundedHub.unsubscribe(slow);
    boundedHub.unsubscribe(fast);

    aegis::daemon::EventHub byteBoundedHub;
    const auto byteSlow = byteBoundedHub.subscribe();
    const std::string largeEvent(1024 * 1024, 'x');
    for (int index = 0; index < 5; ++index)
        byteBoundedHub.publish({"large-" + std::to_string(index), "task", "run", "user.message", "codex", largeEvent, index});
    const auto byteHighWater = byteBoundedHub.highWater(byteSlow);
    assert(byteHighWater.events < aegis::daemon::EventHub::maxQueuedEvents);
    assert(byteHighWater.bytes <= aegis::daemon::EventHub::maxQueuedBytes);
    sawResync = false;
    while (byteBoundedHub.wait(byteSlow, event, std::chrono::milliseconds(1))) {
        if (event.type == "stream.resync_required") { sawResync = true; break; }
    }
    assert(sawResync);
    byteBoundedHub.unsubscribe(byteSlow);
}
