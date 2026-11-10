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
    if (sqlite3_column_type(statement, 7) != SQLITE_NULL) result.model = columnText(statement, 7);
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
    if (sqlite3_column_type(statement, 10) != SQLITE_NULL) result.repositoryFingerprint = columnText(statement, 10);
    result.freshness = "unknown";
    if (sqlite3_column_type(statement, 11) != SQLITE_NULL) result.dispositionReason = columnText(statement, 11);
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

TaskContext readTaskContext(const std::string &raw) {
    nlohmann::json parsed;
    try {
        parsed = nlohmann::json::parse(raw);
    } catch (const nlohmann::json::exception &) {
        throw std::runtime_error("invalid persisted task context");
    }
    if (!parsed.is_object()) throw std::runtime_error("invalid persisted task context");
    const auto field = [&](const char *key) {
        if (!parsed.contains(key)) return std::string{};
        const auto &value = parsed.at(key);
        if (!value.is_string()) throw std::runtime_error("invalid persisted task context");
        return value.get<std::string>();
    };
    return TaskContext{field("goal"), field("constraints"), field("decisions"),
                       field("attempts"), field("unfinished"), field("next_action")};
}

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
        execute("CREATE TABLE IF NOT EXISTS findings (id TEXT PRIMARY KEY, task_id TEXT NOT NULL REFERENCES tasks(id) ON DELETE CASCADE, run_id TEXT REFERENCES runs(id) ON DELETE SET NULL, file_path TEXT NOT NULL, start_line INTEGER, end_line INTEGER, message TEXT NOT NULL, status TEXT NOT NULL CHECK(status IN ('open', 'resolved', 'not_applicable')), repository_fingerprint TEXT, disposition_reason TEXT, created_at INTEGER NOT NULL, updated_at INTEGER NOT NULL, CHECK(start_line IS NULL OR start_line > 0), CHECK(end_line IS NULL OR end_line > 0), CHECK(start_line IS NULL OR end_line IS NULL OR end_line >= start_line), CHECK(status != 'not_applicable' OR disposition_reason IS NOT NULL));");
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
        if (!hasColumn(database_, "verifications", "repository_fingerprint"))
            execute("ALTER TABLE verifications ADD COLUMN repository_fingerprint TEXT;");
        if (previousVersion < 5) execute("PRAGMA user_version = 5;");
        if (!hasColumn(database_, "tasks", "context_json"))
            execute("ALTER TABLE tasks ADD COLUMN context_json TEXT NOT NULL DEFAULT '{}';");
        if (previousVersion < 6) execute("PRAGMA user_version = 6;");
        if (!hasColumn(database_, "findings", "repository_fingerprint"))
            execute("ALTER TABLE findings ADD COLUMN repository_fingerprint TEXT;");
        if (previousVersion < 7) execute("PRAGMA user_version = 7;");
        execute("CREATE TABLE IF NOT EXISTS imports (source_key TEXT PRIMARY KEY, task_id TEXT NOT NULL REFERENCES tasks(id));");
        if (previousVersion < 8) execute("PRAGMA user_version = 8;");
        execute("CREATE INDEX IF NOT EXISTS events_run_type_sequence ON events(run_id, type, sequence DESC);");
        execute("CREATE TABLE IF NOT EXISTS continuations (request_id TEXT PRIMARY KEY, payload TEXT NOT NULL, run_id TEXT REFERENCES runs(id) ON DELETE SET NULL, state TEXT NOT NULL DEFAULT 'prepared');");
        if (previousVersion < 9) execute("PRAGMA user_version = 9;");
        if (!hasColumn(database_, "runs", "model"))
            execute("ALTER TABLE runs ADD COLUMN model TEXT;");
        if (previousVersion < 10) execute("PRAGMA user_version = 10;");
        if (previousVersion < 11) {
            execute("BEGIN IMMEDIATE;");
            try {
                if (!hasColumn(database_, "tasks", "context_revision"))
                    execute("ALTER TABLE tasks ADD COLUMN context_revision INTEGER NOT NULL DEFAULT 0;");
                execute("PRAGMA user_version = 11;");
                execute("COMMIT;");
            } catch (...) {
                execute("ROLLBACK;");
                throw;
            }
        }
        if (!hasColumn(database_, "continuations", "state")) {
            execute("BEGIN IMMEDIATE;");
            try {
                execute("ALTER TABLE continuations ADD COLUMN state TEXT NOT NULL DEFAULT 'uncertain';");
                execute("UPDATE continuations SET state = CASE WHEN run_id IS NOT NULL THEN 'delivered' ELSE 'uncertain' END;");
                execute("PRAGMA user_version = 12;");
                execute("COMMIT;");
            } catch (...) {
                execute("ROLLBACK;");
                throw;
            }
        } else if (previousVersion < 12) execute("PRAGMA user_version = 12;");
        execute("UPDATE continuations SET state = 'uncertain' WHERE state IN ('prepared', 'launching');");
        if (previousVersion < 13) {
            execute("BEGIN IMMEDIATE;");
            try {
                execute("CREATE TABLE IF NOT EXISTS working_profiles (repository TEXT PRIMARY KEY, profile_json TEXT NOT NULL, revision INTEGER NOT NULL);");
                execute("PRAGMA user_version = 13;");
                execute("COMMIT;");
            } catch (...) {
                execute("ROLLBACK;");
                throw;
            }
        }
        if (previousVersion < 14) {
            execute("BEGIN IMMEDIATE;");
            try {
                execute("CREATE TABLE IF NOT EXISTS metadata (key TEXT PRIMARY KEY, value TEXT NOT NULL);");
                execute("INSERT OR IGNORE INTO metadata (key, value) VALUES ('store_origin', lower(hex(randomblob(16))));");
                execute("PRAGMA user_version = 14;");
                execute("COMMIT;");
            } catch (...) {
                execute("ROLLBACK;");
                throw;
            }
        }
        if (previousVersion < 15) {
            execute("BEGIN IMMEDIATE;");
            try {
                execute("ALTER TABLE findings RENAME TO findings_legacy;");
                execute("CREATE TABLE findings (id TEXT PRIMARY KEY, task_id TEXT NOT NULL REFERENCES tasks(id) ON DELETE CASCADE, run_id TEXT REFERENCES runs(id) ON DELETE SET NULL, file_path TEXT NOT NULL, start_line INTEGER, end_line INTEGER, message TEXT NOT NULL, status TEXT NOT NULL CHECK(status IN ('open', 'resolved', 'not_applicable')), repository_fingerprint TEXT, disposition_reason TEXT, created_at INTEGER NOT NULL, updated_at INTEGER NOT NULL, CHECK(start_line IS NULL OR start_line > 0), CHECK(end_line IS NULL OR end_line > 0), CHECK(start_line IS NULL OR end_line IS NULL OR end_line >= start_line), CHECK(status != 'not_applicable' OR disposition_reason IS NOT NULL));");
                execute("INSERT INTO findings (id, task_id, run_id, file_path, start_line, end_line, message, status, created_at, updated_at, repository_fingerprint) SELECT id, task_id, run_id, file_path, start_line, end_line, message, status, created_at, updated_at, repository_fingerprint FROM findings_legacy;");
                execute("DROP TABLE findings_legacy;");
                execute("CREATE INDEX IF NOT EXISTS findings_task_created ON findings(task_id, created_at DESC);");
                execute("PRAGMA user_version = 15;");
                execute("COMMIT;");
            } catch (...) {
                execute("ROLLBACK;");
                throw;
            }
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
    Task task{id("task"), std::move(prompt), std::move(repository), "open", now(), 0};
    Statement statement(database_, "INSERT INTO tasks (id, prompt, repository, status, created_at) VALUES (?, ?, ?, ?, ?)");
    check(sqlite3_bind_text(statement.get(), 1, task.id.c_str(), -1, SQLITE_TRANSIENT), database_, "bind task id");
    check(sqlite3_bind_text(statement.get(), 2, task.prompt.c_str(), -1, SQLITE_TRANSIENT), database_, "bind task prompt");
    check(sqlite3_bind_text(statement.get(), 3, task.repository.c_str(), -1, SQLITE_TRANSIENT), database_, "bind task repository");
    check(sqlite3_bind_text(statement.get(), 4, task.status.c_str(), -1, SQLITE_TRANSIENT), database_, "bind task status");
    check(sqlite3_bind_int64(statement.get(), 5, task.createdAt), database_, "bind task timestamp");
    check(sqlite3_step(statement.get()), database_, "insert task");
    return task;
}

AgentRun Store::startRun(const std::string &taskId, std::string agent, const std::optional<std::string> &model) {
    std::lock_guard lock(mutex_);
    AgentRun run{id("run"), taskId, std::move(agent), "starting", now(), 0, std::nullopt, model};
    Statement statement(database_, "INSERT INTO runs (id, task_id, agent, status, started_at, model) VALUES (?, ?, ?, ?, ?, ?)");
    check(sqlite3_bind_text(statement.get(), 1, run.id.c_str(), -1, SQLITE_TRANSIENT), database_, "bind run id");
    check(sqlite3_bind_text(statement.get(), 2, run.taskId.c_str(), -1, SQLITE_TRANSIENT), database_, "bind run task");
    check(sqlite3_bind_text(statement.get(), 3, run.agent.c_str(), -1, SQLITE_TRANSIENT), database_, "bind run agent");
    check(sqlite3_bind_text(statement.get(), 4, run.status.c_str(), -1, SQLITE_TRANSIENT), database_, "bind run status");
    check(sqlite3_bind_int64(statement.get(), 5, run.startedAt), database_, "bind run timestamp");
    if (run.model)
        check(sqlite3_bind_text(statement.get(), 6, run.model->c_str(), -1, SQLITE_TRANSIENT), database_, "bind run model");
    else check(sqlite3_bind_null(statement.get(), 6), database_, "bind run model");
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
    Statement statement(database_, "SELECT id, task_id, agent, status, started_at, finished_at, external_session_id, model FROM runs WHERE id = ?");
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
    const bool terminalRun = event.type == "run.completed" || event.type == "run.failed" ||
        event.type == "run.interrupted" || event.type == "run.terminated";
    bool pruneTerminalOutput = false;
    if (event.type == "terminal.output") {
        Statement count(database_, "SELECT COUNT(*) FROM events WHERE type = 'terminal.output' AND run_id = ?");
        check(sqlite3_bind_text(count.get(), 1, event.runId.c_str(), -1, SQLITE_TRANSIENT), database_, "bind terminal count run");
        check(sqlite3_step(count.get()), database_, "count terminal output");
        pruneTerminalOutput = sqlite3_column_int(count.get(), 0) >= 160;
    }
    if (pruneTerminalOutput || terminalRun) {
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
    Statement statement(database_, "INSERT INTO verifications (id, task_id, run_id, command_json, exit_code, output, started_at, finished_at, repository_fingerprint) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?)");
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
    if (verification.repositoryFingerprint)
        check(sqlite3_bind_text(statement.get(), 9, verification.repositoryFingerprint->c_str(), -1, SQLITE_TRANSIENT), database_, "bind verification fingerprint");
    else check(sqlite3_bind_null(statement.get(), 9), database_, "bind verification fingerprint");
    check(sqlite3_step(statement.get()), database_, "insert verification");
}

ReviewFinding Store::createFinding(const std::string &taskId, std::optional<std::string> runId,
                                   std::string filePath, std::optional<int> startLine,
                                   std::optional<int> endLine, std::string message,
                                   std::optional<std::string> fingerprint) {
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
                          std::move(message), "open", now(), now(), std::move(fingerprint), "unknown"};
    Statement statement(database_, "INSERT INTO findings (id, task_id, run_id, file_path, start_line, end_line, message, status, created_at, updated_at, repository_fingerprint) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)");
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
    if (finding.repositoryFingerprint)
        check(sqlite3_bind_text(statement.get(), 11, finding.repositoryFingerprint->c_str(), -1, SQLITE_TRANSIENT), database_, "bind finding fingerprint");
    else check(sqlite3_bind_null(statement.get(), 11), database_, "bind finding fingerprint");
    check(sqlite3_step(statement.get()), database_, "insert finding");
    return finding;
}

std::optional<Task> Store::updateTaskContext(const std::string &taskId, const TaskContext &context,
                                             std::optional<std::int64_t> expectedRevision) {
    const auto valid = [](const std::string &value) {
        return value.size() <= 4000 && value.find('\0') == std::string::npos;
    };
    if (!valid(context.goal) || !valid(context.constraints) || !valid(context.decisions) ||
        !valid(context.attempts) || !valid(context.unfinished) || !valid(context.nextAction))
        throw std::invalid_argument("invalid task context fields");
    if (expectedRevision && (*expectedRevision < 0 || *expectedRevision > 9007199254740991LL))
        throw std::invalid_argument("invalid task context revision");
    std::lock_guard lock(mutex_);
    const nlohmann::json encoded{{"goal", context.goal}, {"constraints", context.constraints},
        {"decisions", context.decisions}, {"attempts", context.attempts},
        {"unfinished", context.unfinished}, {"next_action", context.nextAction}};
    const auto encodedText = encoded.dump();
    execute("BEGIN IMMEDIATE;");
    try {
        Statement statement(database_, "UPDATE tasks SET context_json = ?, context_revision = context_revision + 1 "
                                       "WHERE id = ? AND context_revision < 9007199254740991 AND (? IS NULL OR context_revision = ?)");
        check(sqlite3_bind_text(statement.get(), 1, encodedText.c_str(), -1, SQLITE_TRANSIENT), database_, "bind task context");
        check(sqlite3_bind_text(statement.get(), 2, taskId.c_str(), -1, SQLITE_TRANSIENT), database_, "bind task id");
        if (expectedRevision) {
            check(sqlite3_bind_int64(statement.get(), 3, *expectedRevision), database_, "bind expected revision");
            check(sqlite3_bind_int64(statement.get(), 4, *expectedRevision), database_, "bind expected revision");
        } else {
            check(sqlite3_bind_null(statement.get(), 3), database_, "bind expected revision");
            check(sqlite3_bind_null(statement.get(), 4), database_, "bind expected revision");
        }
        check(sqlite3_step(statement.get()), database_, "update task context");
        if (sqlite3_changes(database_) == 0) {
            execute("COMMIT;");
            return std::nullopt;
        }
        Statement updated(database_, "SELECT id, prompt, repository, status, created_at, context_revision, context_json FROM tasks WHERE id = ?");
        check(sqlite3_bind_text(updated.get(), 1, taskId.c_str(), -1, SQLITE_TRANSIENT), database_, "bind task lookup");
        if (sqlite3_step(updated.get()) != SQLITE_ROW) throw std::runtime_error("updated task is missing");
        Task result{columnText(updated.get(), 0), columnText(updated.get(), 1), columnText(updated.get(), 2),
                    columnText(updated.get(), 3), sqlite3_column_int64(updated.get(), 4),
                    sqlite3_column_int64(updated.get(), 5), TaskContext{}};
        result.context = readTaskContext(columnText(updated.get(), 6));
        execute("COMMIT;");
        return result;
    } catch (...) {
        execute("ROLLBACK;");
        throw;
    }
}

bool Store::updateFindingStatus(const std::string &findingId, const std::string &status,
                                std::optional<std::string> reason) {
    if (status != "open" && status != "resolved" && status != "not_applicable") return false;
    if (status == "not_applicable") {
        if (!reason || reason->empty() || reason->size() > 2000 || reason->find('\0') != std::string::npos) return false;
    } else if (reason) {
        return false;
    }
    std::lock_guard lock(mutex_);
    Statement statement(database_, "UPDATE findings SET status = ?, disposition_reason = ?, updated_at = ? WHERE id = ?");
    check(sqlite3_bind_text(statement.get(), 1, status.c_str(), -1, SQLITE_TRANSIENT), database_, "bind finding status");
    if (reason) check(sqlite3_bind_text(statement.get(), 2, reason->c_str(), -1, SQLITE_TRANSIENT), database_, "bind finding disposition reason");
    else check(sqlite3_bind_null(statement.get(), 2), database_, "bind finding disposition reason");
    check(sqlite3_bind_int64(statement.get(), 3, now()), database_, "bind finding timestamp");
    check(sqlite3_bind_text(statement.get(), 4, findingId.c_str(), -1, SQLITE_TRANSIENT), database_, "bind finding id");
    check(sqlite3_step(statement.get()), database_, "update finding status");
    return sqlite3_changes(database_) != 0;
}

std::optional<Task> Store::task(const std::string &taskId) const {
    std::lock_guard lock(mutex_);
    Statement statement(database_, "SELECT id, prompt, repository, status, created_at, context_revision, context_json FROM tasks WHERE id = ?");
    check(sqlite3_bind_text(statement.get(), 1, taskId.c_str(), -1, SQLITE_TRANSIENT), database_, "bind task lookup");
    if (sqlite3_step(statement.get()) != SQLITE_ROW) return std::nullopt;
    Task result{reinterpret_cast<const char *>(sqlite3_column_text(statement.get(), 0)),
                reinterpret_cast<const char *>(sqlite3_column_text(statement.get(), 1)),
                reinterpret_cast<const char *>(sqlite3_column_text(statement.get(), 2)),
                reinterpret_cast<const char *>(sqlite3_column_text(statement.get(), 3)),
                sqlite3_column_int64(statement.get(), 4), sqlite3_column_int64(statement.get(), 5), TaskContext{}};
    result.context = readTaskContext(columnText(statement.get(), 6));
    return result;
}

std::vector<AgentRun> Store::runs(const std::string &taskId) const {
    std::lock_guard lock(mutex_);
    Statement statement(database_, "SELECT id, task_id, agent, status, started_at, finished_at, external_session_id, model FROM runs WHERE task_id = ? ORDER BY started_at, rowid");
    check(sqlite3_bind_text(statement.get(), 1, taskId.c_str(), -1, SQLITE_TRANSIENT), database_, "bind run task");
    std::vector<AgentRun> result;
    while (sqlite3_step(statement.get()) == SQLITE_ROW) {
        result.push_back(*readRun(statement.get()));
    }
    return result;
}

std::optional<AgentEvent> Store::latestTurnEvent(const std::string &runId) const {
    std::lock_guard lock(mutex_);
    Statement statement(database_, "SELECT id, task_id, run_id, type, agent, content, timestamp, sequence FROM events WHERE run_id = ? AND type IN ('turn.started', 'turn.completed', 'turn.interrupted') ORDER BY sequence DESC LIMIT 1");
    check(sqlite3_bind_text(statement.get(), 1, runId.c_str(), -1, SQLITE_TRANSIENT), database_, "bind turn run");
    if (sqlite3_step(statement.get()) != SQLITE_ROW) return std::nullopt;
    return AgentEvent{columnText(statement.get(), 0), columnText(statement.get(), 1), columnText(statement.get(), 2),
                      columnText(statement.get(), 3), columnText(statement.get(), 4), columnText(statement.get(), 5),
                      sqlite3_column_int64(statement.get(), 6), sqlite3_column_int64(statement.get(), 7)};
}

std::vector<VerificationRun> Store::verifications(const std::string &taskId, std::size_t limit) const {
    std::lock_guard lock(mutex_);
    Statement statement(database_, "SELECT id, task_id, run_id, command_json, exit_code, output, started_at, finished_at, repository_fingerprint FROM verifications WHERE task_id = ? ORDER BY finished_at DESC LIMIT ?");
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
        if (sqlite3_column_type(statement.get(), 8) != SQLITE_NULL) verification.repositoryFingerprint = columnText(statement.get(), 8);
        result.push_back(std::move(verification));
    }
    return result;
}

std::vector<ReviewFinding> Store::findings(const std::string &taskId, std::size_t limit) const {
    std::lock_guard lock(mutex_);
    Statement statement(database_, "SELECT id, task_id, run_id, file_path, start_line, end_line, message, status, created_at, updated_at, repository_fingerprint, disposition_reason FROM findings WHERE task_id = ? ORDER BY CASE WHEN status = 'open' THEN 0 ELSE 1 END, created_at DESC LIMIT ?");
    check(sqlite3_bind_text(statement.get(), 1, taskId.c_str(), -1, SQLITE_TRANSIENT), database_, "bind finding task");
    check(sqlite3_bind_int64(statement.get(), 2, static_cast<sqlite3_int64>(limit)), database_, "bind finding limit");
    std::vector<ReviewFinding> result;
    while (sqlite3_step(statement.get()) == SQLITE_ROW) result.push_back(readFinding(statement.get()));
    return result;
}

std::vector<Task> Store::tasks() const {
    std::lock_guard lock(mutex_);
    Statement statement(database_, "SELECT id, prompt, repository, status, created_at, context_revision, context_json FROM tasks ORDER BY created_at");
    std::vector<Task> result;
    while (sqlite3_step(statement.get()) == SQLITE_ROW) {
        Task task{reinterpret_cast<const char *>(sqlite3_column_text(statement.get(), 0)),
                  reinterpret_cast<const char *>(sqlite3_column_text(statement.get(), 1)),
                  reinterpret_cast<const char *>(sqlite3_column_text(statement.get(), 2)),
                  reinterpret_cast<const char *>(sqlite3_column_text(statement.get(), 3)),
                  sqlite3_column_int64(statement.get(), 4), sqlite3_column_int64(statement.get(), 5), TaskContext{}};
        task.context = readTaskContext(columnText(statement.get(), 6));
        result.push_back(std::move(task));
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

Task Store::importTaskBundle(const std::string &bundleJson, const std::string &destinationRepository) {
    if (bundleJson.size() > 1024 * 1024) throw std::invalid_argument("task bundle exceeds size limit");
    if (destinationRepository.empty() || destinationRepository.find('\0') != std::string::npos)
        throw std::invalid_argument("invalid destination repository");
    nlohmann::json bundle;
    try {
        bundle = nlohmann::json::parse(bundleJson);
    } catch (const nlohmann::json::exception &) {
        throw std::invalid_argument("invalid task bundle JSON");
    }
    const auto requireString = [](const nlohmann::json &parent, const char *key, std::size_t maxLength,
                                  bool nonBlank) {
        if (!parent.is_object() || !parent.contains(key) || !parent.at(key).is_string())
            throw std::invalid_argument(std::string("invalid task bundle field: ") + key);
        const auto value = parent.at(key).get<std::string>();
        if (value.size() > maxLength || value.find('\0') != std::string::npos ||
            (nonBlank && value.find_first_not_of(" \t\r\n") == std::string::npos))
            throw std::invalid_argument(std::string("invalid task bundle field: ") + key);
        return value;
    };
    if (!bundle.is_object() || !bundle.contains("format") || !bundle.at("format").is_string() ||
        bundle.at("format").get<std::string>() != "aegis-task" || !bundle.contains("version") ||
        !bundle.at("version").is_number_integer() || bundle.at("version") != 1)
        throw std::invalid_argument("invalid task bundle format or version");
    const auto origin = requireString(bundle, "source_origin", 200, true);
    const auto sourceTaskId = requireString(bundle, "source_task_id", 200, true);
    const auto sourceRepository = requireString(bundle, "source_repository", 4000, false);
    const auto prompt = requireString(bundle, "prompt", 8000, true);
    if (!bundle.contains("context") || !bundle.at("context").is_object())
        throw std::invalid_argument("invalid task bundle context");
    const auto &contextJson = bundle.at("context");
    if (contextJson.size() != 6 || !contextJson.contains("goal") || !contextJson.contains("constraints") ||
        !contextJson.contains("decisions") || !contextJson.contains("attempts") ||
        !contextJson.contains("unfinished") || !contextJson.contains("next_action"))
        throw std::invalid_argument("invalid task bundle context");
    TaskContext context;
    context.goal = requireString(contextJson, "goal", 4000, false);
    context.constraints = requireString(contextJson, "constraints", 4000, false);
    context.decisions = requireString(contextJson, "decisions", 4000, false);
    context.attempts = requireString(contextJson, "attempts", 4000, false);
    context.unfinished = requireString(contextJson, "unfinished", 4000, false);
    context.nextAction = requireString(contextJson, "next_action", 4000, false);
    if (!bundle.contains("messages") || !bundle.at("messages").is_array() ||
        bundle.at("messages").size() > 1000)
        throw std::invalid_argument("invalid task bundle messages");
    for (const auto &message : bundle.at("messages")) {
        if (!message.is_object()) throw std::invalid_argument("invalid task bundle message record");
        requireString(message, "id", 4000, true);
        requireString(message, "run_id", 4000, false);
        requireString(message, "agent", 200, false);
        const auto messageType = requireString(message, "type", 200, true);
        if (messageType != "user.message" && messageType != "assistant.message" && messageType != "import.message")
            throw std::invalid_argument("invalid task bundle message type");
        requireString(message, "content", 65536, false);
        if (!message.contains("timestamp") || !message.at("timestamp").is_number_integer())
            throw std::invalid_argument("invalid task bundle message timestamp");
        if (message.contains("sequence") && !message.at("sequence").is_number_integer())
            throw std::invalid_argument("invalid task bundle message sequence");
    }
    if (!bundle.contains("findings") || !bundle.at("findings").is_array() ||
        bundle.at("findings").size() > 100)
        throw std::invalid_argument("invalid task bundle findings");
    for (const auto &finding : bundle.at("findings")) {
        if (!finding.is_object()) throw std::invalid_argument("invalid task bundle finding record");
        requireString(finding, "id", 4000, true);
        requireString(finding, "file_path", 4000, false);
        requireString(finding, "message", 8000, true);
        requireString(finding, "status", 100, false);
    }
    if (!bundle.contains("verifications") || !bundle.at("verifications").is_array() ||
        bundle.at("verifications").size() > 20)
        throw std::invalid_argument("invalid task bundle verifications");
    for (const auto &verification : bundle.at("verifications")) {
        if (!verification.is_object()) throw std::invalid_argument("invalid task bundle verification record");
        if (verification.dump().size() > 8000) throw std::invalid_argument("task bundle verification too large");
    }
    nlohmann::json profile = nlohmann::json{{"version", 1}, {"instructions", nlohmann::json::array()},
                                            {"workflows", nlohmann::json::array()}};
    if (bundle.contains("profile")) {
        const auto &supplied = bundle.at("profile");
        if (!supplied.is_object() || !supplied.contains("version") ||
            !supplied.at("version").is_number_integer() || supplied.at("version") != 1 ||
            !supplied.contains("instructions") || !supplied.at("instructions").is_array() ||
            !supplied.contains("workflows") || !supplied.at("workflows").is_array())
            throw std::invalid_argument("invalid task bundle profile");
        profile = supplied;
        if (profile.dump().size() > 8000) throw std::invalid_argument("task bundle profile too large");
    }
    std::vector<ImportedMessage> imported;
    imported.reserve(bundle.at("messages").size() + bundle.at("findings").size() +
                     bundle.at("verifications").size() + 1);
    std::size_t index = 0;
    for (const auto &message : bundle.at("messages")) {
        ImportedMessage record;
        record.id = "message:" + message.at("id").get<std::string>();
        const auto type = message.at("type").get<std::string>();
        record.role = type == "assistant.message" ? "assistant"
                      : (type == "import.message" ? "tool" : "user");
        record.text = message.at("content").get<std::string>();
        record.sourceName = "Aegis " + origin + " run " + message.at("run_id").get<std::string>();
        record.sourceLine = message.at("sequence").is_number_integer() &&
                                    message.at("sequence").get<std::int64_t>() > 0
                                ? message.at("sequence").get<std::int64_t>()
                                : static_cast<std::int64_t>(index + 1);
        imported.push_back(std::move(record));
        ++index;
    }
    const auto evidence = [&](const nlohmann::json &original, const char *kind) {
        ImportedMessage record;
        record.id = "message:" + origin + ":" + sourceTaskId + ":" + kind + ":" +
                    std::to_string(imported.size());
        record.role = "tool";
        nlohmann::json annotated = original;
        annotated["source_origin"] = origin;
        annotated["source_task_id"] = sourceTaskId;
        annotated["kind"] = kind;
        record.text = annotated.dump();
        record.sourceName = "Aegis " + origin + " task " + sourceTaskId;
        record.sourceLine = static_cast<std::int64_t>(imported.size() + 1);
        imported.push_back(std::move(record));
    };
    for (const auto &finding : bundle.at("findings")) evidence(finding, "finding");
    for (const auto &verification : bundle.at("verifications")) evidence(verification, "verification");
    ImportedMessage metadata;
    metadata.id = "message:" + origin + ":" + sourceTaskId + ":bundle";
    metadata.role = "tool";
    metadata.text = nlohmann::json{{"kind", "bundle"}, {"source_origin", origin},
        {"source_task_id", sourceTaskId}, {"source_repository", sourceRepository},
        {"profile", profile}, {"omissions", nlohmann::json::array()}, {"historical", true}}.dump();
    metadata.sourceName = "Aegis " + origin + " task " + sourceTaskId;
    metadata.sourceLine = static_cast<std::int64_t>(imported.size() + 1);
    imported.push_back(std::move(metadata));
    const auto sourceKey = nlohmann::json{"aegis-task", origin, sourceTaskId, destinationRepository}.dump();
    return importTask(sourceKey, prompt, destinationRepository, context, imported);
}

Task Store::importTask(const std::string &sourceKey, std::string prompt, std::string repository,
                       const TaskContext &context, const std::vector<ImportedMessage> &messages) {
    std::lock_guard lock(mutex_);
    execute("BEGIN IMMEDIATE;");
    try {
        std::string existingTaskId;
        {
            Statement lookup(database_, "SELECT task_id FROM imports WHERE source_key = ?");
            check(sqlite3_bind_text(lookup.get(), 1, sourceKey.c_str(), -1, SQLITE_TRANSIENT), database_, "bind source key");
            if (sqlite3_step(lookup.get()) == SQLITE_ROW) existingTaskId = columnText(lookup.get(), 0);
        }
        if (!existingTaskId.empty()) {
            Statement statement(database_, "SELECT id, prompt, repository, status, created_at, context_revision, context_json FROM tasks WHERE id = ?");
            check(sqlite3_bind_text(statement.get(), 1, existingTaskId.c_str(), -1, SQLITE_TRANSIENT), database_, "bind existing task lookup");
            if (sqlite3_step(statement.get()) != SQLITE_ROW) throw std::runtime_error("import points to missing task");
            Task result{columnText(statement.get(), 0), columnText(statement.get(), 1), columnText(statement.get(), 2),
                        columnText(statement.get(), 3), sqlite3_column_int64(statement.get(), 4),
                        sqlite3_column_int64(statement.get(), 5), TaskContext{}};
            result.context = readTaskContext(columnText(statement.get(), 6));
            execute("COMMIT;");
            return result;
        }
        Task task{id("task"), std::move(prompt), std::move(repository), "open", now(), 0, context};
        const nlohmann::json encoded{{"goal", context.goal}, {"constraints", context.constraints},
            {"decisions", context.decisions}, {"attempts", context.attempts},
            {"unfinished", context.unfinished}, {"next_action", context.nextAction}};
        const auto encodedContext = encoded.dump();
        {
            Statement statement(database_, "INSERT INTO tasks (id, prompt, repository, status, created_at, context_json) VALUES (?, ?, ?, ?, ?, ?)");
            check(sqlite3_bind_text(statement.get(), 1, task.id.c_str(), -1, SQLITE_TRANSIENT), database_, "bind task id");
            check(sqlite3_bind_text(statement.get(), 2, task.prompt.c_str(), -1, SQLITE_TRANSIENT), database_, "bind task prompt");
            check(sqlite3_bind_text(statement.get(), 3, task.repository.c_str(), -1, SQLITE_TRANSIENT), database_, "bind task repository");
            check(sqlite3_bind_text(statement.get(), 4, task.status.c_str(), -1, SQLITE_TRANSIENT), database_, "bind task status");
            check(sqlite3_bind_int64(statement.get(), 5, task.createdAt), database_, "bind task timestamp");
            check(sqlite3_bind_text(statement.get(), 6, encodedContext.c_str(), -1, SQLITE_TRANSIENT), database_, "bind task context");
            check(sqlite3_step(statement.get()), database_, "insert task");
        }
        AgentRun run{id("run"), task.id, "import", "completed", now(), now(), std::nullopt};
        {
            Statement statement(database_, "INSERT INTO runs (id, task_id, agent, status, started_at, finished_at) VALUES (?, ?, ?, ?, ?, ?)");
            check(sqlite3_bind_text(statement.get(), 1, run.id.c_str(), -1, SQLITE_TRANSIENT), database_, "bind run id");
            check(sqlite3_bind_text(statement.get(), 2, run.taskId.c_str(), -1, SQLITE_TRANSIENT), database_, "bind run task");
            check(sqlite3_bind_text(statement.get(), 3, run.agent.c_str(), -1, SQLITE_TRANSIENT), database_, "bind run agent");
            check(sqlite3_bind_text(statement.get(), 4, run.status.c_str(), -1, SQLITE_TRANSIENT), database_, "bind run status");
            check(sqlite3_bind_int64(statement.get(), 5, run.startedAt), database_, "bind run timestamp");
            check(sqlite3_bind_int64(statement.get(), 6, run.finishedAt), database_, "bind run finish");
            check(sqlite3_step(statement.get()), database_, "insert run");
        }
        const auto insertEvent = [&](const char *type, const std::string &content) {
            AgentEvent event{id("event"), task.id, run.id, type, "import", content, now(), 0};
            Statement statement(database_, "INSERT INTO events (id, task_id, run_id, type, agent, content, timestamp) VALUES (?, ?, ?, ?, ?, ?, ?)");
            check(sqlite3_bind_text(statement.get(), 1, event.id.c_str(), -1, SQLITE_TRANSIENT), database_, "bind event id");
            check(sqlite3_bind_text(statement.get(), 2, event.taskId.c_str(), -1, SQLITE_TRANSIENT), database_, "bind event task");
            check(sqlite3_bind_text(statement.get(), 3, event.runId.c_str(), -1, SQLITE_TRANSIENT), database_, "bind event run");
            check(sqlite3_bind_text(statement.get(), 4, event.type.c_str(), -1, SQLITE_TRANSIENT), database_, "bind event type");
            check(sqlite3_bind_text(statement.get(), 5, event.agent.c_str(), -1, SQLITE_TRANSIENT), database_, "bind event agent");
            check(sqlite3_bind_text(statement.get(), 6, event.content.c_str(), -1, SQLITE_TRANSIENT), database_, "bind event content");
            check(sqlite3_bind_int64(statement.get(), 7, event.timestamp), database_, "bind event timestamp");
            check(sqlite3_step(statement.get()), database_, "insert event");
        };
        for (const auto &message : messages)
            insertEvent("import.message", nlohmann::json{{"source_id", message.id}, {"role", message.role},
                {"text", message.text}, {"source_line", message.sourceLine}, {"source_name", message.sourceName},
                {"historical", true}}.dump());
        insertEvent("task.context", encodedContext);
        insertEvent("import.source", nlohmann::json{{"source_key", sourceKey}}.dump());
        {
            Statement statement(database_, "INSERT INTO imports (source_key, task_id) VALUES (?, ?)");
            check(sqlite3_bind_text(statement.get(), 1, sourceKey.c_str(), -1, SQLITE_TRANSIENT), database_, "bind import source key");
            check(sqlite3_bind_text(statement.get(), 2, task.id.c_str(), -1, SQLITE_TRANSIENT), database_, "bind import task id");
            check(sqlite3_step(statement.get()), database_, "insert import");
        }
        execute("COMMIT;");
        return task;
    } catch (...) {
        execute("ROLLBACK;");
        throw;
    }
}

std::string Store::exportTaskBundle(const std::string &taskId) const {
    std::string storeOrigin;
    {
        std::lock_guard lock(mutex_);
        Statement statement(database_, "SELECT value FROM metadata WHERE key = 'store_origin'");
        if (sqlite3_step(statement.get()) == SQLITE_ROW) storeOrigin = columnText(statement.get(), 0);
    }
    const auto taskOpt = task(taskId);
    if (!taskOpt) throw std::invalid_argument("task not found: " + taskId);
    const auto &source = *taskOpt;
    if (source.prompt.size() > 8000) throw std::invalid_argument("task prompt exceeds 8000 bytes");
    const auto contextField = [](const std::string &value) { return value.size() <= 4000; };
    if (!contextField(source.context.goal) || !contextField(source.context.constraints) ||
        !contextField(source.context.decisions) || !contextField(source.context.attempts) ||
        !contextField(source.context.unfinished) || !contextField(source.context.nextAction))
        throw std::invalid_argument("task context field exceeds 4000 bytes");

    auto recentEvents = events(taskId, 1001, false);
    const bool eventsOmitted = recentEvents.size() > 1000;
    if (eventsOmitted) recentEvents.erase(recentEvents.begin());
    nlohmann::json messages = nlohmann::json::array();
    for (const auto &event : recentEvents) {
        if (event.type != "user.message" && event.type != "assistant.message" && event.type != "import.message")
            continue;
        messages.push_back({{"id", event.id}, {"run_id", event.runId}, {"agent", event.agent},
            {"type", event.type}, {"content", event.content}, {"timestamp", event.timestamp},
            {"sequence", event.sequence}});
    }

    const auto allFindings = findings(taskId, 101);
    const bool findingsOmitted = allFindings.size() > 100;
    nlohmann::json findingsJson = nlohmann::json::array();
    for (std::size_t index = 0; index < allFindings.size() && index < 100; ++index) {
        const auto &finding = allFindings[index];
        findingsJson.push_back({{"id", finding.id},
            {"file_path", finding.filePath},
            {"start_line", finding.startLine ? nlohmann::json(*finding.startLine) : nlohmann::json(nullptr)},
            {"end_line", finding.endLine ? nlohmann::json(*finding.endLine) : nlohmann::json(nullptr)},
            {"message", finding.message}, {"status", finding.status},
            {"repository_fingerprint", finding.repositoryFingerprint ? nlohmann::json(*finding.repositoryFingerprint) : nlohmann::json(nullptr)},
            {"freshness", "historical"}});
    }

    const auto allVerifications = verifications(taskId, 21);
    const bool verificationsOmitted = allVerifications.size() > 20;
    nlohmann::json verificationsJson = nlohmann::json::array();
    for (std::size_t index = 0; index < allVerifications.size() && index < 20; ++index) {
        const auto &verification = allVerifications[index];
        verificationsJson.push_back({{"id", verification.id}, {"command", verification.command},
            {"exit_code", verification.exitCode}, {"output", verification.output},
            {"repository_fingerprint", verification.repositoryFingerprint ? nlohmann::json(*verification.repositoryFingerprint) : nlohmann::json(nullptr)},
            {"freshness", "historical"}});
    }

    nlohmann::json profile = nlohmann::json::object();
    try {
        profile = nlohmann::json::parse(workingProfile(source.repository).first);
    } catch (const nlohmann::json::exception &) {
        profile = nlohmann::json::object();
    }

    nlohmann::json omissions = nlohmann::json::array();
    if (eventsOmitted) omissions.push_back("History bounded to most recent 1000 semantic events");
    if (findingsOmitted) omissions.push_back("Findings bounded to most recent 100");
    if (verificationsOmitted) omissions.push_back("Verifications bounded to most recent 20");

    nlohmann::json bundle{
        {"format", "aegis-task"},
        {"version", 1},
        {"source_origin", storeOrigin},
        {"source_task_id", source.id},
        {"source_repository", source.repository},
        {"prompt", source.prompt},
        {"context", {{"goal", source.context.goal}, {"constraints", source.context.constraints},
            {"decisions", source.context.decisions}, {"attempts", source.context.attempts},
            {"unfinished", source.context.unfinished}, {"next_action", source.context.nextAction}}},
        {"profile", profile},
        {"messages", messages},
        {"findings", findingsJson},
        {"verifications", verificationsJson},
        {"omissions", omissions},
        {"historical", true}};
    const auto serialized = bundle.dump();
    if (serialized.size() > 1048576) throw std::invalid_argument("task bundle exceeds 1048576 bytes");
    return serialized;
}

bool Store::reserveContinuation(const std::string &requestId, const std::string &payload) {
    std::lock_guard lock(mutex_);
    Statement statement(database_, "INSERT OR IGNORE INTO continuations (request_id, payload, state) VALUES (?, ?, 'prepared')");
    check(sqlite3_bind_text(statement.get(), 1, requestId.c_str(), -1, SQLITE_TRANSIENT), database_, "bind continuation id");
    check(sqlite3_bind_text(statement.get(), 2, payload.c_str(), -1, SQLITE_TRANSIENT), database_, "bind continuation payload");
    check(sqlite3_step(statement.get()), database_, "insert continuation");
    return sqlite3_changes(database_) != 0;
}

std::optional<std::pair<std::string, std::optional<std::string>>> Store::continuation(const std::string &requestId) const {
    std::lock_guard lock(mutex_);
    Statement statement(database_, "SELECT payload, run_id FROM continuations WHERE request_id = ?");
    check(sqlite3_bind_text(statement.get(), 1, requestId.c_str(), -1, SQLITE_TRANSIENT), database_, "bind continuation lookup");
    if (sqlite3_step(statement.get()) != SQLITE_ROW) return std::nullopt;
    std::optional<std::string> runId;
    if (sqlite3_column_type(statement.get(), 1) != SQLITE_NULL) runId = columnText(statement.get(), 1);
    return std::make_pair(columnText(statement.get(), 0), runId);
}

bool Store::completeContinuation(const std::string &requestId, const std::string &runId) {
    std::lock_guard lock(mutex_);
    Statement statement(database_, "UPDATE continuations SET run_id = ?, state = 'delivered' WHERE request_id = ? AND state IN ('prepared', 'launching')");
    check(sqlite3_bind_text(statement.get(), 1, runId.c_str(), -1, SQLITE_TRANSIENT), database_, "bind continuation run");
    check(sqlite3_bind_text(statement.get(), 2, requestId.c_str(), -1, SQLITE_TRANSIENT), database_, "bind continuation id");
    check(sqlite3_step(statement.get()), database_, "update continuation run");
    return sqlite3_changes(database_) != 0;
}

std::optional<std::string> Store::continuationState(const std::string &requestId) const {
    std::lock_guard lock(mutex_);
    Statement statement(database_, "SELECT state FROM continuations WHERE request_id = ?");
    check(sqlite3_bind_text(statement.get(), 1, requestId.c_str(), -1, SQLITE_TRANSIENT), database_, "bind continuation lookup");
    if (sqlite3_step(statement.get()) != SQLITE_ROW) return std::nullopt;
    return columnText(statement.get(), 0);
}

bool Store::linkContinuation(const std::string &requestId, const std::string &runId) {
    std::lock_guard lock(mutex_);
    execute("BEGIN IMMEDIATE;");
    try {
        std::string payload;
        {
            Statement statement(database_, "SELECT payload FROM continuations WHERE request_id = ? AND state = 'prepared'");
            check(sqlite3_bind_text(statement.get(), 1, requestId.c_str(), -1, SQLITE_TRANSIENT), database_, "bind continuation lookup");
            if (sqlite3_step(statement.get()) != SQLITE_ROW) {
                execute("COMMIT;");
                return false;
            }
            payload = columnText(statement.get(), 0);
        }
        std::string taskId;
        try {
            const auto parsed = nlohmann::json::parse(payload);
            if (parsed.is_object() && parsed.contains("task_id") && parsed.at("task_id").is_string())
                taskId = parsed.at("task_id").get<std::string>();
        } catch (const nlohmann::json::exception &) {
            execute("COMMIT;");
            return false;
        }
        if (taskId.empty()) {
            execute("COMMIT;");
            return false;
        }
        {
            Statement run(database_, "SELECT task_id FROM runs WHERE id = ?");
            check(sqlite3_bind_text(run.get(), 1, runId.c_str(), -1, SQLITE_TRANSIENT), database_, "bind run lookup");
            if (sqlite3_step(run.get()) != SQLITE_ROW || columnText(run.get(), 0) != taskId) {
                execute("COMMIT;");
                return false;
            }
        }
        Statement statement(database_, "UPDATE continuations SET run_id = ?, state = 'launching' WHERE request_id = ? AND state = 'prepared'");
        check(sqlite3_bind_text(statement.get(), 1, runId.c_str(), -1, SQLITE_TRANSIENT), database_, "bind continuation run");
        check(sqlite3_bind_text(statement.get(), 2, requestId.c_str(), -1, SQLITE_TRANSIENT), database_, "bind continuation id");
        check(sqlite3_step(statement.get()), database_, "update continuation link");
        const bool linked = sqlite3_changes(database_) != 0;
        execute("COMMIT;");
        return linked;
    } catch (...) {
        execute("ROLLBACK;");
        throw;
    }
}

std::pair<std::string, std::int64_t> Store::workingProfile(const std::string &repository) const {
    if (repository.empty() || repository.find('\0') != std::string::npos)
        throw std::invalid_argument("invalid repository");
    std::lock_guard lock(mutex_);
    static const std::string emptyProfile = nlohmann::json{{"version", 1}, {"instructions", nlohmann::json::array()}, {"workflows", nlohmann::json::array()}}.dump();
    Statement statement(database_, "SELECT profile_json, revision FROM working_profiles WHERE repository = ?");
    check(sqlite3_bind_text(statement.get(), 1, repository.c_str(), -1, SQLITE_TRANSIENT), database_, "bind working profile lookup");
    if (sqlite3_step(statement.get()) != SQLITE_ROW) return {emptyProfile, 0};
    return {columnText(statement.get(), 0), sqlite3_column_int64(statement.get(), 1)};
}

std::optional<std::int64_t> Store::saveWorkingProfile(const std::string &repository,
                                                      const std::string &profileJson, std::int64_t expectedRevision) {
    if (repository.empty() || repository.find('\0') != std::string::npos)
        throw std::invalid_argument("invalid repository");
    if (expectedRevision < 0 || expectedRevision > 9007199254740991LL)
        throw std::invalid_argument("invalid working profile revision");
    if (profileJson.size() > 8000 || profileJson.find('\0') != std::string::npos)
        throw std::invalid_argument("invalid working profile");
    {
        nlohmann::json parsed;
        try {
            parsed = nlohmann::json::parse(profileJson);
        } catch (const nlohmann::json::exception &) {
            throw std::invalid_argument("invalid working profile");
        }
        if (!parsed.is_object() || !parsed.contains("version") || !parsed.at("version").is_number_integer() ||
            parsed.at("version") != 1 || !parsed.contains("instructions") || !parsed.at("instructions").is_array() ||
            !parsed.contains("workflows") || !parsed.at("workflows").is_array())
            throw std::invalid_argument("invalid working profile");
    }
    std::lock_guard lock(mutex_);
    execute("BEGIN IMMEDIATE;");
    try {
        std::int64_t currentRevision = -1;
        {
            Statement current(database_, "SELECT revision FROM working_profiles WHERE repository = ?");
            check(sqlite3_bind_text(current.get(), 1, repository.c_str(), -1, SQLITE_TRANSIENT), database_, "bind working profile lookup");
            if (sqlite3_step(current.get()) == SQLITE_ROW) currentRevision = sqlite3_column_int64(current.get(), 0);
        }
        const bool exists = currentRevision >= 0;
        if ((exists && currentRevision != expectedRevision) || (!exists && expectedRevision != 0)) {
            execute("COMMIT;");
            return std::nullopt;
        }
        if (exists) {
            Statement statement(database_, "UPDATE working_profiles SET profile_json = ?, revision = revision + 1 WHERE repository = ? AND revision = ?");
            check(sqlite3_bind_text(statement.get(), 1, profileJson.c_str(), -1, SQLITE_TRANSIENT), database_, "bind working profile");
            check(sqlite3_bind_text(statement.get(), 2, repository.c_str(), -1, SQLITE_TRANSIENT), database_, "bind working profile repository");
            check(sqlite3_bind_int64(statement.get(), 3, expectedRevision), database_, "bind working profile revision");
            check(sqlite3_step(statement.get()), database_, "update working profile");
            execute("COMMIT;");
            return expectedRevision + 1;
        }
        Statement statement(database_, "INSERT INTO working_profiles (repository, profile_json, revision) VALUES (?, ?, 1)");
        check(sqlite3_bind_text(statement.get(), 1, repository.c_str(), -1, SQLITE_TRANSIENT), database_, "bind working profile repository");
        check(sqlite3_bind_text(statement.get(), 2, profileJson.c_str(), -1, SQLITE_TRANSIENT), database_, "bind working profile");
        check(sqlite3_step(statement.get()), database_, "insert working profile");
        execute("COMMIT;");
        return 1;
    } catch (...) {
        execute("ROLLBACK;");
        throw;
    }
}

bool Store::failContinuation(const std::string &requestId) {
    std::lock_guard lock(mutex_);
    Statement statement(database_, "UPDATE continuations SET state = 'failed' WHERE request_id = ? AND state IN ('prepared', 'launching')");
    check(sqlite3_bind_text(statement.get(), 1, requestId.c_str(), -1, SQLITE_TRANSIENT), database_, "bind continuation id");
    check(sqlite3_step(statement.get()), database_, "fail continuation");
    return sqlite3_changes(database_) != 0;
}

}
