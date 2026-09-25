#include "daemon/session/store.h"

#include <sqlite3.h>

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
    static std::uint64_t sequence = 0;
    return std::string(prefix) + "-" + std::to_string(now()) + "-" + std::to_string(++sequence);
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

}

Store::Store(const std::filesystem::path &path) {
    if (sqlite3_open(path.string().c_str(), &database_) != SQLITE_OK) {
        const auto message = database_ ? sqlite3_errmsg(database_) : "could not open database";
        if (database_) sqlite3_close(database_);
        database_ = nullptr;
        throw std::runtime_error(message);
    }
    execute("PRAGMA foreign_keys = ON;");
    execute("CREATE TABLE IF NOT EXISTS tasks (id TEXT PRIMARY KEY, prompt TEXT NOT NULL, repository TEXT NOT NULL, status TEXT NOT NULL, created_at INTEGER NOT NULL);");
    execute("CREATE TABLE IF NOT EXISTS runs (id TEXT PRIMARY KEY, task_id TEXT NOT NULL REFERENCES tasks(id), agent TEXT NOT NULL, status TEXT NOT NULL, started_at INTEGER NOT NULL, finished_at INTEGER NOT NULL DEFAULT 0);");
    execute("CREATE TABLE IF NOT EXISTS events (id TEXT PRIMARY KEY, task_id TEXT NOT NULL REFERENCES tasks(id), run_id TEXT NOT NULL, type TEXT NOT NULL, agent TEXT NOT NULL, content TEXT NOT NULL, timestamp INTEGER NOT NULL);");
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
    AgentRun run{id("run"), taskId, std::move(agent), "starting", now(), 0};
    Statement statement(database_, "INSERT INTO runs (id, task_id, agent, status, started_at) VALUES (?, ?, ?, ?, ?)");
    check(sqlite3_bind_text(statement.get(), 1, run.id.c_str(), -1, SQLITE_TRANSIENT), database_, "bind run id");
    check(sqlite3_bind_text(statement.get(), 2, run.taskId.c_str(), -1, SQLITE_TRANSIENT), database_, "bind run task");
    check(sqlite3_bind_text(statement.get(), 3, run.agent.c_str(), -1, SQLITE_TRANSIENT), database_, "bind run agent");
    check(sqlite3_bind_text(statement.get(), 4, run.status.c_str(), -1, SQLITE_TRANSIENT), database_, "bind run status");
    check(sqlite3_bind_int64(statement.get(), 5, run.startedAt), database_, "bind run timestamp");
    check(sqlite3_step(statement.get()), database_, "insert run");
    return run;
}

void Store::appendEvent(const AgentEvent &event) {
    Statement statement(database_, "INSERT INTO events (id, task_id, run_id, type, agent, content, timestamp) VALUES (?, ?, ?, ?, ?, ?, ?)");
    const char *values[] = {event.id.c_str(), event.taskId.c_str(), event.runId.c_str(), event.type.c_str(), event.agent.c_str(), event.content.c_str()};
    for (int index = 0; index < 6; ++index)
        check(sqlite3_bind_text(statement.get(), index + 1, values[index], -1, SQLITE_TRANSIENT), database_, "bind event");
    check(sqlite3_bind_int64(statement.get(), 7, event.timestamp), database_, "bind event timestamp");
    check(sqlite3_step(statement.get()), database_, "insert event");
}

std::vector<Task> Store::tasks() const {
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
