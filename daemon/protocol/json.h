#pragma once

#include "daemon/domain/types.h"

#include <nlohmann/json.hpp>

namespace aegis::daemon::protocol {

inline nlohmann::json toJson(const Task &task) {
    return {{"id", task.id}, {"prompt", task.prompt}, {"repository", task.repository}, {"status", task.status}, {"created_at", task.createdAt}};
}

inline nlohmann::json toJson(const AgentRun &run) {
    return {{"id", run.id}, {"task_id", run.taskId}, {"agent", run.agent}, {"status", run.status}, {"started_at", run.startedAt}, {"finished_at", run.finishedAt}};
}

inline nlohmann::json toJson(const AgentEvent &event) {
    return {{"id", event.id}, {"task_id", event.taskId}, {"run_id", event.runId}, {"type", event.type}, {"agent", event.agent}, {"content", event.content}, {"timestamp", event.timestamp}};
}

inline nlohmann::json toJson(const RepositoryState &state) {
    return {{"path", state.path}, {"branch", state.branch}, {"files", state.files}, {"insertions", state.insertions}, {"deletions", state.deletions}};
}

inline nlohmann::json toJson(const VerificationRun &run) {
    return {{"id", run.id}, {"command", run.command}, {"exit_code", run.exitCode}, {"output", run.output}, {"started_at", run.startedAt}, {"finished_at", run.finishedAt}};
}

}
