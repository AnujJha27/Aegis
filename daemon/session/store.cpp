#include "daemon/session/store.h"

#include <nlohmann/json.hpp>
#include <sqlite3.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <stdexcept>

namespace aegis::daemon {
namespace {

std::int64_t now() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

std::string id(const char *prefix) {
    static std::atomic_uint64_t sequence = 0;
    return std::string(prefix) + "-" + std::to_string(now()) + "-" + std::to_string(++sequence);
}

std::string columnText(sqlite3_stmt *statement, int column) {
    const auto *value = sqlite3_column_text(statement, column);
    return value ? reinterpret_cast<const char *>(value) : std::string{};
}

bool terminal(const std::string &status) {
    return status == "completed" || status == "failed" || status == "interrupted" || status == "terminated";
}

std::optional<AgentRun> readRun(sqlite3_stmt *statement) {
    if (sqlite3_column_type(statement, 0) == SQLITE_NULL) return std::nullopt;
    AgentRun result{columnText(statement, 0), columnText(statement, 1), columnText(statement, 2),
                    columnText(statement, 3), sqlite3_column_int64(statement, 4), sqlite3_column_int64(statement, 5), std::nullopt};
    if (sqlite3_column_type(statement, 6) != SQLITE_NULL) result.externalSessionId = columnText(statement, 6);
    return result;
}

ReviewFinding readFinding(sqlite3_stmt *statement) {
    ReviewFinding result;
    result.id = columnText(statement, 0);
    result.taskId = columnText(statement, 1);
    if (sqlite3_column_type(statement, 2) != SQLITE_NULL) result.runId = columnText(statement, 2);
    result.filePath = columnText(statement, 3);
    if (sqlite3_column_type(statement, 4) != SQLITE_NULL) result.startLine = sqlite3_column_int(statement, 4);
    if (sqlite3_column_type(statement, 5) != SQLITE_NULL) result.endLine = sqlite3_column_int(statement, 5);
    result.message = columnText(statement, 6);
    result.status = columnText(statement, 7);
    result.createdAt = sqlite3_column_int64(statement, 8);
    result.updatedAt = sqlite3_column_int64(statement, 9);
    return result;
}

void check(int result, sqlite3 *database, const char *operation) {
    if (result == SQLITE_OK || result == SQLITE_DONE || result == SQLITE_ROW) return;
    throw std::runtime_error(std::string(operation) + ": " + sqlite3_errmsg(database));
}

class Statement final {
public:
    Statement(sqlite3 *database, const char *sql) : database_(database) {
        check(sqlite3_prepare_v2(database, sql, -1, &statement_, nullptr), database, "prepare");
    }
    ~Statement() { sqlite3_finalize(statement_); }
    sqlite3_stmt *get() const { return statement_; }

private:
    sqlite3 *database_;
    sqlite3_stmt *statement_ = nullptr;
};

bool hasColumn(sqlite3 *database, const char *table, const char *column) {
    const auto sql = std::string("PRAGMA table_info(") + table + ")";
    Statement statement(database, sql.c_str());
    while (sqlite3_step(statement.get()) == SQLITE_ROW)
        if (columnText(statement.get(), 1) == column) return true;
    return false;
}

}

Store::Store(const std::filesystem::path &path) {
    if (sqlite3_open(path.string().c_str(), &database_) != SQLITE_OK) {
        const auto message = database_ ? sqlite3_errmsg(database_) : "could not open database";
        if (database_) sqlite3_close(database_);
        database_ = nullptr;
        throw std::runtime_error("database " + path.string() + ": " + message);
    }
    try {
        int previousVersion = 0;
        {
            Statement schemaVersion(database_, "PRAGMA user_version");
            check(sqlite3_step(schemaVersion.get()), database_, "read schema version");
            previousVersion = sqlite3_column_int(schemaVersion.get(), 0);
        }
        if (previousVersion > currentSchemaVersion) throw std::runtime_error("database schema is newer than this Aegis build");
        execute("PRAGMA foreign_keys = ON;");
        execute("CREATE TABLE IF NOT EXISTS tasks (id TEXT PRIMARY KEY, prompt TEXT NOT NULL, repository TEXT NOT NULL, status TEXT NOT NULL, created_at INTEGER NOT NULL);");
        execute("CREATE TABLE IF NOT EXISTS runs (id TEXT PRIMARY KEY, task_id TEXT NOT NULL REFERENCES tasks(id), agent TEXT NOT NULL, status TEXT NOT NULL, started_at INTEGER NOT NULL, finished_at INTEGER NOT NULL DEFAULT 0);");
        execute("CREATE TABLE IF NOT EXISTS events (id TEXT PRIMARY KEY, task_id TEXT NOT NULL REFERENCES tasks(id), run_id TEXT NOT NULL, type TEXT NOT NULL, agent TEXT NOT NULL, content TEXT NOT NULL, timestamp INTEGER NOT NULL);");
        execute("CREATE INDEX IF NOT EXISTS events_task_timestamp ON events(task_id, timestamp);");
        if (!hasColumn(database_, "runs", "external_session_id"))
            execute("ALTER TABLE runs ADD COLUMN external_session_id TEXT;");
        execute("CREATE TABLE IF NOT EXISTS verifications (id TEXT PRIMARY KEY, task_id TEXT NOT NULL REFERENCES tasks(id), run_id TEXT REFERENCES runs(id) ON DELETE SET NULL, command_json TEXT NOT NULL, exit_code INTEGER NOT NULL, output TEXT NOT NULL, started_at INTEGER NOT NULL, finished_at INTEGER NOT NULL);");
        execute("CREATE INDEX IF NOT EXISTS verifications_task_finished ON verifications(task_id, finished_at DESC);");
        execute("CREATE TABLE IF NOT EXISTS findings (id TEXT PRIMARY KEY, task_id TEXT NOT NULL REFERENCES tasks(id) ON DELETE CASCADE, run_id TEXT REFERENCES runs(id) ON DELETE SET NULL, file_path TEXT NOT NULL, start_line INTEGER, end_line INTEGER, message TEXT NOT NULL, status TEXT NOT NULL CHECK(status IN ('open', 'resolved')), created_at INTEGER NOT NULL, updated_at INTEGER NOT NULL, CHECK(start_line IS NULL OR start_line > 0), CHECK(end_line IS NULL OR end_line > 0), CHECK(start_line IS NULL OR end_line IS NULL OR end_line >= start_line));");
        execute("CREATE INDEX IF NOT EXISTS findings_task_created ON findings(task_id, created_at DESC);");
        execute("UPDATE runs SET status = 'interrupted', finished_at = CAST((julianday('now') - 2440587.5) * 86400000 AS INTEGER) WHERE status IN ('starting', 'running') AND finished_at = 0;");
        if (previousVersion < 3) execute("PRAGMA user_version = 3;");
        if (previousVersion < 4) {
            execute("BEGIN IMMEDIATE;");
            execute("ALTER TABLE events RENAME TO events_legacy;");
            execute("CREATE TABLE events (sequence INTEGER PRIMARY KEY AUTOINCREMENT, id TEXT NOT NULL UNIQUE, task_id TEXT NOT NULL REFERENCES tasks(id), run_id TEXT NOT NULL, type TEXT NOT NULL, agent TEXT NOT NULL, content TEXT NOT NULL, timestamp INTEGER NOT NULL);");
            execute("INSERT INTO events (id, task_id, run_id, type, agent, content, timestamp) SELECT id, task_id, run_id, type, agent, content, timestamp FROM events_legacy ORDER BY rowid;");
            execute("DROP TABLE events_legacy;");
            execute("CREATE INDEX events_task_sequence ON events(task_id, sequence);");
            execute("PRAGMA user_version = 4;");
            execute("COMMIT;");
        }
    } catch (const std::exception &error) {
        sqlite3_close(database_);
        database_ = nullptr;
        throw std::runtime_error("database " + path.string() + ": " + error.what());
    }
}

Store::~Store() {
    if (database_) sqlite3_close(database_);
}

void Store::execute(const char *sql) const {
    char *error = nullptr;
    const auto result = sqlite3_exec(database_, sql, nullptr, nullptr, &error);
    if (result == SQLITE_OK) return;
    const std::string message = error ? error : "sqlite error";
    sqlite3_free(error);
    throw std::runtime_error(message);
}

Task Store::createTask(std::string prompt, std::string repository) {
    std::lock_guard lock(mutex_);
    Task task{id("task"), std::move(prompt), std::move(repository), "open", now()};
    Statement statement(database_, "INSERT INTO tasks (id, prompt, repository, status, created_at) VALUES (?, ?, ?, ?, ?)");
    check(sqlite3_bind_text(statement.get(), 1, task.id.c_str(), -1, SQLITE_TRANSIENT), database_, "bind task id");
    check(sqlite3_bind_text(statement.get(), 2, task.prompt.c_str(), -1, SQLITE_TRANSIENT), database_, "bind task prompt");
    check(sqlite3_bind_text(statement.get(), 3, task.repository.c_str(), -1, SQLITE_TRANSIENT), database_, "bind task repository");
    check(sqlite3_bind_text(statement.get(), 4, task.status.c_str(), -1, SQLITE_TRANSIENT), database_, "bind task status");
    check(sqlite3_bind_int64(statement.get(), 5, task.createdAt), database_, "bind task timestamp");
    check(sqlite3_step(statement.get()), database_, "insert task");
    return task;
}

AgentRun Store::startRun(const std::string &taskId, std::string agent) {
    std::lock_guard lock(mutex_);
    AgentRun run{id("run"), taskId, std::move(agent), "starting", now(), 0, std::nullopt};
    Statement statement(database_, "INSERT INTO runs (id, task_id, agent, status, started_at) VALUES (?, ?, ?, ?, ?)");
    check(sqlite3_bind_text(statement.get(), 1, run.id.c_str(), -1, SQLITE_TRANSIENT), database_, "bind run id");
    check(sqlite3_bind_text(statement.get(), 2, run.taskId.c_str(), -1, SQLITE_TRANSIENT), database_, "bind run task");
    check(sqlite3_bind_text(statement.get(), 3, run.agent.c_str(), -1, SQLITE_TRANSIENT), database_, "bind run agent");
    check(sqlite3_bind_text(statement.get(), 4, run.status.c_str(), -1, SQLITE_TRANSIENT), database_, "bind run status");
    check(sqlite3_bind_int64(statement.get(), 5, run.startedAt), database_, "bind run timestamp");
    check(sqlite3_step(statement.get()), database_, "insert run");
    return run;
}

bool Store::updateRunStatus(const std::string &runId, const std::string &status) {
    if (status != "starting" && status != "running" && !terminal(status)) return false;
    std::lock_guard lock(mutex_);
    Statement current(database_, "SELECT status FROM runs WHERE id = ?");
    check(sqlite3_bind_text(current.get(), 1, runId.c_str(), -1, SQLITE_TRANSIENT), database_, "bind run id");
    if (sqlite3_step(current.get()) != SQLITE_ROW) return false;
    const auto previous = columnText(current.get(), 0);
    if (previous == status) return true;
    if (terminal(previous) || (status == "starting" && previous != "starting") ||
        (status == "running" && previous != "starting")) return false;
    Statement update(database_, "UPDATE runs SET status = ?, finished_at = ? WHERE id = ?");
    check(sqlite3_bind_text(update.get(), 1, status.c_str(), -1, SQLITE_TRANSIENT), database_, "bind run status");
    check(sqlite3_bind_int64(update.get(), 2, terminal(status) ? now() : 0), database_, "bind run finished timestamp");
    check(sqlite3_bind_text(update.get(), 3, runId.c_str(), -1, SQLITE_TRANSIENT), database_, "bind run id");
    check(sqlite3_step(update.get()), database_, "update run status");
    return sqlite3_changes(database_) != 0;
}

bool Store::setExternalSessionId(const std::string &runId, std::string sessionId) {
    std::lock_guard lock(mutex_);
    Statement statement(database_, "UPDATE runs SET external_session_id = ? WHERE id = ?");
    check(sqlite3_bind_text(statement.get(), 1, sessionId.c_str(), -1, SQLITE_TRANSIENT), database_, "bind external session id");
    check(sqlite3_bind_text(statement.get(), 2, runId.c_str(), -1, SQLITE_TRANSIENT), database_, "bind run id");
    check(sqlite3_step(statement.get()), database_, "save external session id");
    return sqlite3_changes(database_) != 0;
}

std::optional<AgentRun> Store::run(const std::string &runId) const {
    std::lock_guard lock(mutex_);
    Statement statement(database_, "SELECT id, task_id, agent, status, started_at, finished_at, external_session_id FROM runs WHERE id = ?");
    check(sqlite3_bind_text(statement.get(), 1, runId.c_str(), -1, SQLITE_TRANSIENT), database_, "bind run lookup");
    if (sqlite3_step(statement.get()) != SQLITE_ROW) return std::nullopt;
    return readRun(statement.get());
}

bool Store::deleteRun(const std::string &runId) {
    std::lock_guard lock(mutex_);
    execute("BEGIN IMMEDIATE;");
    try {
        Statement events(database_, "DELETE FROM events WHERE run_id = ?");
        check(sqlite3_bind_text(events.get(), 1, runId.c_str(), -1, SQLITE_TRANSIENT), database_, "bind run events");
        check(sqlite3_step(events.get()), database_, "delete run events");

        Statement run(database_, "DELETE FROM runs WHERE id = ?");
        check(sqlite3_bind_text(run.get(), 1, runId.c_str(), -1, SQLITE_TRANSIENT), database_, "bind run id");
        check(sqlite3_step(run.get()), database_, "delete run");
        const bool deleted = sqlite3_changes(database_) != 0;
        execute("COMMIT;");
        return deleted;
    } catch (...) {
        execute("ROLLBACK;");
        throw;
    }
}

void Store::appendEvent(const AgentEvent &event) {
    std::lock_guard lock(mutex_);
    Statement statement(database_, "INSERT INTO events (id, task_id, run_id, type, agent, content, timestamp) VALUES (?, ?, ?, ?, ?, ?, ?)");
    const char *values[] = {event.id.c_str(), event.taskId.c_str(), event.runId.c_str(), event.type.c_str(), event.agent.c_str(), event.content.c_str()};
    for (int index = 0; index < 6; ++index)
        check(sqlite3_bind_text(statement.get(), index + 1, values[index], -1, SQLITE_TRANSIENT), database_, "bind event");
    check(sqlite3_bind_int64(statement.get(), 7, event.timestamp), database_, "bind event timestamp");
    check(sqlite3_step(statement.get()), database_, "insert event");
    const auto rowId = sqlite3_last_insert_rowid(database_);
    const bool terminalRun = event.type == "run.completed" || event.type == "run.failed" ||
        event.type == "run.interrupted" || event.type == "run.terminated";
    if ((event.type == "terminal.output" && rowId % 32 == 0) || terminalRun) {
        const auto retainCount = 128;
        Statement retain(database_, "DELETE FROM events WHERE type = 'terminal.output' AND run_id = ? AND rowid NOT IN (SELECT rowid FROM events WHERE type = 'terminal.output' AND run_id = ? ORDER BY rowid DESC LIMIT ?)");
        check(sqlite3_bind_text(retain.get(), 1, event.runId.c_str(), -1, SQLITE_TRANSIENT), database_, "bind terminal retention run");
        check(sqlite3_bind_text(retain.get(), 2, event.runId.c_str(), -1, SQLITE_TRANSIENT), database_, "bind terminal retention run");
        check(sqlite3_bind_int(retain.get(), 3, retainCount), database_, "bind terminal retention limit");
        check(sqlite3_step(retain.get()), database_, "retain terminal output");
    }
}

void Store::saveVerification(const VerificationRun &verification) {
    std::lock_guard lock(mutex_);
    const auto command = nlohmann::json(verification.command).dump();
    Statement statement(database_, "INSERT INTO verifications (id, task_id, run_id, command_json, exit_code, output, started_at, finished_at) VALUES (?, ?, ?, ?, ?, ?, ?, ?)");
    check(sqlite3_bind_text(statement.get(), 1, verification.id.c_str(), -1, SQLITE_TRANSIENT), database_, "bind verification id");
    check(sqlite3_bind_text(statement.get(), 2, verification.taskId.c_str(), -1, SQLITE_TRANSIENT), database_, "bind verification task");
    if (verification.runId)
        check(sqlite3_bind_text(statement.get(), 3, verification.runId->c_str(), -1, SQLITE_TRANSIENT), database_, "bind verification run");
    else check(sqlite3_bind_null(statement.get(), 3), database_, "bind empty verification run");
    check(sqlite3_bind_text(statement.get(), 4, command.c_str(), -1, SQLITE_TRANSIENT), database_, "bind verification command");
    check(sqlite3_bind_int(statement.get(), 5, verification.exitCode), database_, "bind verification exit code");
    check(sqlite3_bind_text(statement.get(), 6, verification.output.c_str(), -1, SQLITE_TRANSIENT), database_, "bind verification output");
    check(sqlite3_bind_int64(statement.get(), 7, verification.startedAt), database_, "bind verification start");
    check(sqlite3_bind_int64(statement.get(), 8, verification.finishedAt), database_, "bind verification finish");
    check(sqlite3_step(statement.get()), database_, "insert verification");
}

ReviewFinding Store::createFinding(const std::string &taskId, std::optional<std::string> runId,
                                   std::string filePath, std::optional<int> startLine,
                                   std::optional<int> endLine, std::string message) {
    if (filePath.empty() || message.empty() || (startLine && *startLine <= 0) ||
        (endLine && *endLine <= 0) || (startLine && endLine && *endLine < *startLine))
        throw std::invalid_argument("invalid review finding fields");
    std::lock_guard lock(mutex_);
    if (runId) {
        Statement runStatement(database_, "SELECT task_id FROM runs WHERE id = ?");
        check(sqlite3_bind_text(runStatement.get(), 1, runId->c_str(), -1, SQLITE_TRANSIENT), database_, "bind finding run");
        if (sqlite3_step(runStatement.get()) != SQLITE_ROW || columnText(runStatement.get(), 0) != taskId)
            throw std::invalid_argument("finding run must belong to its task");
    }
    ReviewFinding finding{id("finding"), taskId, std::move(runId), std::move(filePath), startLine, endLine,
                          std::move(message), "open", now(), now()};
    Statement statement(database_, "INSERT INTO findings (id, task_id, run_id, file_path, start_line, end_line, message, status, created_at, updated_at) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?)");
    const auto bindText = [&](int index, const std::string &value) {
        check(sqlite3_bind_text(statement.get(), index, value.c_str(), -1, SQLITE_TRANSIENT), database_, "bind finding");
    };
    bindText(1, finding.id);
    bindText(2, finding.taskId);
    if (finding.runId) bindText(3, *finding.runId);
    else check(sqlite3_bind_null(statement.get(), 3), database_, "bind finding run");
    bindText(4, finding.filePath);
    if (finding.startLine) check(sqlite3_bind_int(statement.get(), 5, *finding.startLine), database_, "bind finding start line");
    else check(sqlite3_bind_null(statement.get(), 5), database_, "bind finding start line");
    if (finding.endLine) check(sqlite3_bind_int(statement.get(), 6, *finding.endLine), database_, "bind finding end line");
    else check(sqlite3_bind_null(statement.get(), 6), database_, "bind finding end line");
    bindText(7, finding.message);
    bindText(8, finding.status);
    check(sqlite3_bind_int64(statement.get(), 9, finding.createdAt), database_, "bind finding timestamp");
    check(sqlite3_bind_int64(statement.get(), 10, finding.updatedAt), database_, "bind finding timestamp");
    check(sqlite3_step(statement.get()), database_, "insert finding");
    return finding;
}

bool Store::updateFindingStatus(const std::string &findingId, const std::string &status) {
    if (status != "open" && status != "resolved") return false;
    std::lock_guard lock(mutex_);
    Statement statement(database_, "UPDATE findings SET status = ?, updated_at = ? WHERE id = ?");
    check(sqlite3_bind_text(statement.get(), 1, status.c_str(), -1, SQLITE_TRANSIENT), database_, "bind finding status");
    check(sqlite3_bind_int64(statement.get(), 2, now()), database_, "bind finding timestamp");
    check(sqlite3_bind_text(statement.get(), 3, findingId.c_str(), -1, SQLITE_TRANSIENT), database_, "bind finding id");
    check(sqlite3_step(statement.get()), database_, "update finding status");
    return sqlite3_changes(database_) != 0;
}

std::optional<Task> Store::task(const std::string &taskId) const {
    std::lock_guard lock(mutex_);
    Statement statement(database_, "SELECT id, prompt, repository, status, created_at FROM tasks WHERE id = ?");
    check(sqlite3_bind_text(statement.get(), 1, taskId.c_str(), -1, SQLITE_TRANSIENT), database_, "bind task lookup");
    if (sqlite3_step(statement.get()) != SQLITE_ROW) return std::nullopt;
    return Task{reinterpret_cast<const char *>(sqlite3_column_text(statement.get(), 0)),
                reinterpret_cast<const char *>(sqlite3_column_text(statement.get(), 1)),
                reinterpret_cast<const char *>(sqlite3_column_text(statement.get(), 2)),
                reinterpret_cast<const char *>(sqlite3_column_text(statement.get(), 3)),
                sqlite3_column_int64(statement.get(), 4)};
}

std::vector<AgentRun> Store::runs(const std::string &taskId) const {
    std::lock_guard lock(mutex_);
    Statement statement(database_, "SELECT id, task_id, agent, status, started_at, finished_at, external_session_id FROM runs WHERE task_id = ? ORDER BY started_at, rowid");
    check(sqlite3_bind_text(statement.get(), 1, taskId.c_str(), -1, SQLITE_TRANSIENT), database_, "bind run task");
    std::vector<AgentRun> result;
    while (sqlite3_step(statement.get()) == SQLITE_ROW) {
        result.push_back(*readRun(statement.get()));
    }
    return result;
}

std::vector<VerificationRun> Store::verifications(const std::string &taskId, std::size_t limit) const {
    std::lock_guard lock(mutex_);
    Statement statement(database_, "SELECT id, task_id, run_id, command_json, exit_code, output, started_at, finished_at FROM verifications WHERE task_id = ? ORDER BY finished_at DESC LIMIT ?");
    check(sqlite3_bind_text(statement.get(), 1, taskId.c_str(), -1, SQLITE_TRANSIENT), database_, "bind verification task");
    check(sqlite3_bind_int64(statement.get(), 2, static_cast<sqlite3_int64>(limit)), database_, "bind verification limit");
    std::vector<VerificationRun> result;
    while (sqlite3_step(statement.get()) == SQLITE_ROW) {
        VerificationRun verification;
        verification.id = columnText(statement.get(), 0);
        verification.taskId = columnText(statement.get(), 1);
        if (sqlite3_column_type(statement.get(), 2) != SQLITE_NULL) verification.runId = columnText(statement.get(), 2);
        try { verification.command = nlohmann::json::parse(columnText(statement.get(), 3)).get<std::vector<std::string>>(); }
        catch (const nlohmann::json::exception &) { throw std::runtime_error("invalid persisted verification command"); }
        verification.exitCode = sqlite3_column_int(statement.get(), 4);
        verification.output = columnText(statement.get(), 5);
        verification.startedAt = sqlite3_column_int64(statement.get(), 6);
        verification.finishedAt = sqlite3_column_int64(statement.get(), 7);
        result.push_back(std::move(verification));
    }
    return result;
}

std::vector<ReviewFinding> Store::findings(const std::string &taskId, std::size_t limit) const {
    std::lock_guard lock(mutex_);
    Statement statement(database_, "SELECT id, task_id, run_id, file_path, start_line, end_line, message, status, created_at, updated_at FROM findings WHERE task_id = ? ORDER BY created_at DESC LIMIT ?");
    check(sqlite3_bind_text(statement.get(), 1, taskId.c_str(), -1, SQLITE_TRANSIENT), database_, "bind finding task");
    check(sqlite3_bind_int64(statement.get(), 2, static_cast<sqlite3_int64>(limit)), database_, "bind finding limit");
    std::vector<ReviewFinding> result;
    while (sqlite3_step(statement.get()) == SQLITE_ROW) result.push_back(readFinding(statement.get()));
    return result;
}

std::vector<Task> Store::tasks() const {
    std::lock_guard lock(mutex_);
    Statement statement(database_, "SELECT id, prompt, repository, status, created_at FROM tasks ORDER BY created_at");
    std::vector<Task> result;
    while (sqlite3_step(statement.get()) == SQLITE_ROW) {
        result.push_back({reinterpret_cast<const char *>(sqlite3_column_text(statement.get(), 0)),
                          reinterpret_cast<const char *>(sqlite3_column_text(statement.get(), 1)),
                          reinterpret_cast<const char *>(sqlite3_column_text(statement.get(), 2)),
                          reinterpret_cast<const char *>(sqlite3_column_text(statement.get(), 3)),
                          sqlite3_column_int64(statement.get(), 4)});
    }
    return result;
}

std::vector<AgentEvent> Store::events(const std::string &taskId, std::optional<std::size_t> limit, bool includeTerminalOutput) const {
    std::lock_guard lock(mutex_);
    const char *query = includeTerminalOutput
        ? (limit ? "SELECT id, task_id, run_id, type, agent, content, timestamp, sequence FROM events WHERE task_id = ? ORDER BY sequence DESC LIMIT ?"
                 : "SELECT id, task_id, run_id, type, agent, content, timestamp, sequence FROM events WHERE task_id = ? ORDER BY sequence")
        : (limit ? "SELECT id, task_id, run_id, type, agent, content, timestamp, sequence FROM events WHERE task_id = ? AND type != 'terminal.output' ORDER BY sequence DESC LIMIT ?"
                 : "SELECT id, task_id, run_id, type, agent, content, timestamp, sequence FROM events WHERE task_id = ? AND type != 'terminal.output' ORDER BY sequence");
    Statement statement(database_, query);
    check(sqlite3_bind_text(statement.get(), 1, taskId.c_str(), -1, SQLITE_TRANSIENT), database_, "bind event task");
    if (limit) check(sqlite3_bind_int64(statement.get(), 2, static_cast<sqlite3_int64>(*limit)), database_, "bind event limit");
    std::vector<AgentEvent> result;
    while (sqlite3_step(statement.get()) == SQLITE_ROW) {
        result.push_back({reinterpret_cast<const char *>(sqlite3_column_text(statement.get(), 0)),
                          reinterpret_cast<const char *>(sqlite3_column_text(statement.get(), 1)),
                          reinterpret_cast<const char *>(sqlite3_column_text(statement.get(), 2)),
                          reinterpret_cast<const char *>(sqlite3_column_text(statement.get(), 3)),
                          reinterpret_cast<const char *>(sqlite3_column_text(statement.get(), 4)),
                          reinterpret_cast<const char *>(sqlite3_column_text(statement.get(), 5)),
                          sqlite3_column_int64(statement.get(), 6), sqlite3_column_int64(statement.get(), 7)});
    }
    if (limit) std::reverse(result.begin(), result.end());
    return result;
}

AgentEventPage Store::eventsBefore(const std::string &taskId, std::int64_t sequence, std::size_t limit) const {
    std::lock_guard lock(mutex_);
    limit = std::clamp<std::size_t>(limit, 1, 500);
    Statement statement(database_, "SELECT id, task_id, run_id, type, agent, content, timestamp, sequence FROM events WHERE task_id = ? AND sequence < ? AND type != 'terminal.output' ORDER BY sequence DESC LIMIT ?");
    check(sqlite3_bind_text(statement.get(), 1, taskId.c_str(), -1, SQLITE_TRANSIENT), database_, "bind event task");
    check(sqlite3_bind_int64(statement.get(), 2, sequence), database_, "bind event cursor");
    check(sqlite3_bind_int64(statement.get(), 3, static_cast<sqlite3_int64>(limit + 1)), database_, "bind event page size");
    AgentEventPage page;
    while (sqlite3_step(statement.get()) == SQLITE_ROW) {
        page.events.push_back({columnText(statement.get(), 0), columnText(statement.get(), 1), columnText(statement.get(), 2),
                               columnText(statement.get(), 3), columnText(statement.get(), 4), columnText(statement.get(), 5),
                               sqlite3_column_int64(statement.get(), 6), sqlite3_column_int64(statement.get(), 7)});
    }
    page.hasMore = page.events.size() > limit;
    if (page.hasMore) page.events.pop_back();
    std::reverse(page.events.begin(), page.events.end());
    if (!page.events.empty()) page.nextCursor = page.events.front().sequence;
    return page;
}

std::vector<AgentEvent> Store::terminalOutput(const std::string &runId, std::size_t limit) const {
    std::lock_guard lock(mutex_);
    limit = std::clamp<std::size_t>(limit, 1, 128);
    Statement statement(database_, "SELECT id, task_id, run_id, type, agent, content, timestamp, sequence FROM events WHERE run_id = ? AND type = 'terminal.output' ORDER BY sequence DESC LIMIT ?");
    check(sqlite3_bind_text(statement.get(), 1, runId.c_str(), -1, SQLITE_TRANSIENT), database_, "bind terminal run");
    check(sqlite3_bind_int64(statement.get(), 2, static_cast<sqlite3_int64>(limit)), database_, "bind terminal limit");
    std::vector<AgentEvent> result;
    while (sqlite3_step(statement.get()) == SQLITE_ROW)
        result.push_back({columnText(statement.get(), 0), columnText(statement.get(), 1), columnText(statement.get(), 2),
                          columnText(statement.get(), 3), columnText(statement.get(), 4), columnText(statement.get(), 5),
                          sqlite3_column_int64(statement.get(), 6), sqlite3_column_int64(statement.get(), 7)});
    std::reverse(result.begin(), result.end());
    return result;
}

}
