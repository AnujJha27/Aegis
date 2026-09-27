#include "daemon/session/store.h"

#include <nlohmann/json.hpp>
#include <sqlite3.h>

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
        Statement schemaVersion(database_, "PRAGMA user_version");
        check(sqlite3_step(schemaVersion.get()), database_, "read schema version");
        const auto previousVersion = sqlite3_column_int(schemaVersion.get(), 0);
        if (previousVersion > 1) throw std::runtime_error("database schema is newer than this Aegis build");
        execute("PRAGMA foreign_keys = ON;");
        execute("CREATE TABLE IF NOT EXISTS tasks (id TEXT PRIMARY KEY, prompt TEXT NOT NULL, repository TEXT NOT NULL, status TEXT NOT NULL, created_at INTEGER NOT NULL);");
        execute("CREATE TABLE IF NOT EXISTS runs (id TEXT PRIMARY KEY, task_id TEXT NOT NULL REFERENCES tasks(id), agent TEXT NOT NULL, status TEXT NOT NULL, started_at INTEGER NOT NULL, finished_at INTEGER NOT NULL DEFAULT 0);");
        execute("CREATE TABLE IF NOT EXISTS events (id TEXT PRIMARY KEY, task_id TEXT NOT NULL REFERENCES tasks(id), run_id TEXT NOT NULL, type TEXT NOT NULL, agent TEXT NOT NULL, content TEXT NOT NULL, timestamp INTEGER NOT NULL);");
        if (!hasColumn(database_, "runs", "external_session_id"))
            execute("ALTER TABLE runs ADD COLUMN external_session_id TEXT;");
        execute("CREATE TABLE IF NOT EXISTS verifications (id TEXT PRIMARY KEY, task_id TEXT NOT NULL REFERENCES tasks(id), run_id TEXT REFERENCES runs(id) ON DELETE SET NULL, command_json TEXT NOT NULL, exit_code INTEGER NOT NULL, output TEXT NOT NULL, started_at INTEGER NOT NULL, finished_at INTEGER NOT NULL);");
        execute("CREATE INDEX IF NOT EXISTS verifications_task_finished ON verifications(task_id, finished_at DESC);");
        execute("UPDATE runs SET status = 'interrupted', finished_at = CAST((julianday('now') - 2440587.5) * 86400000 AS INTEGER) WHERE status IN ('starting', 'running') AND finished_at = 0;");
        execute("PRAGMA user_version = 1;");
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

std::vector<AgentEvent> Store::events(const std::string &taskId) const {
    std::lock_guard lock(mutex_);
    Statement statement(database_, "SELECT id, task_id, run_id, type, agent, content, timestamp FROM events WHERE task_id = ? ORDER BY timestamp, rowid");
    check(sqlite3_bind_text(statement.get(), 1, taskId.c_str(), -1, SQLITE_TRANSIENT), database_, "bind event task");
    std::vector<AgentEvent> result;
    while (sqlite3_step(statement.get()) == SQLITE_ROW) {
        result.push_back({reinterpret_cast<const char *>(sqlite3_column_text(statement.get(), 0)),
                          reinterpret_cast<const char *>(sqlite3_column_text(statement.get(), 1)),
                          reinterpret_cast<const char *>(sqlite3_column_text(statement.get(), 2)),
                          reinterpret_cast<const char *>(sqlite3_column_text(statement.get(), 3)),
                          reinterpret_cast<const char *>(sqlite3_column_text(statement.get(), 4)),
                          reinterpret_cast<const char *>(sqlite3_column_text(statement.get(), 5)),
                          sqlite3_column_int64(statement.get(), 6)});
    }
    return result;
}

}
