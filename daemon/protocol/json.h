#pragma once

#include "daemon/domain/types.h"

#include <nlohmann/json.hpp>

namespace aegis::daemon::protocol {

inline nlohmann::json toJson(const Task &task) {
    return {{"id", task.id}, {"prompt", task.prompt}, {"repository", task.repository}, {"status", task.status}, {"created_at", task.createdAt}};
}

inline nlohmann::json toJson(const AgentRun &run) {
    return {{"id", run.id}, {"task_id", run.taskId}, {"agent", run.agent}, {"status", run.status}, {"started_at", run.startedAt}, {"finished_at", run.finishedAt}, {"external_session_id", run.externalSessionId ? nlohmann::json(*run.externalSessionId) : nlohmann::json(nullptr)}};
}

inline nlohmann::json toJson(const AgentEvent &event) {
    return {{"id", event.id}, {"task_id", event.taskId}, {"run_id", event.runId}, {"type", event.type}, {"agent", event.agent}, {"content", event.content}, {"timestamp", event.timestamp}};
}

inline nlohmann::json toJson(const RepositoryState &state) {
    return {{"path", state.path}, {"branch", state.branch}, {"files", state.files}, {"insertions", state.insertions}, {"deletions", state.deletions}};
}

inline nlohmann::json toJson(const GitChange &change) {
    return {{"path", change.path}, {"index_status", change.indexStatus}, {"worktree_status", change.worktreeStatus}};
}

inline nlohmann::json toJson(const VerificationRun &run) {
    return {{"id", run.id}, {"task_id", run.taskId}, {"run_id", run.runId ? nlohmann::json(*run.runId) : nlohmann::json(nullptr)}, {"command", run.command}, {"exit_code", run.exitCode}, {"output", run.output}, {"started_at", run.startedAt}, {"finished_at", run.finishedAt}};
}

inline nlohmann::json toJson(const HandoffContext &context) {
    nlohmann::json events = nlohmann::json::array();
    for (const auto &event : context.recentEvents) events.push_back(toJson(event));
    nlohmann::json result{{"task_id", context.taskId}, {"prompt", context.prompt}, {"recent_events", events}, {"diff", context.diff}, {"changed_files", context.changedFiles}};
    result["verification"] = context.verification ? toJson(*context.verification) : nlohmann::json(nullptr);
    return result;
}

inline nlohmann::json toJson(const GraphNode &node) {
    return {{"id", node.id}, {"type", node.type}, {"label", node.label}};
}

inline nlohmann::json toJson(const GraphEdge &edge) {
    return {{"from", edge.from}, {"to", edge.to}};
}

inline nlohmann::json toJson(const TaskGraph &graph) {
    nlohmann::json nodes = nlohmann::json::array();
    nlohmann::json edges = nlohmann::json::array();
    for (const auto &node : graph.nodes) nodes.push_back(toJson(node));
    for (const auto &edge : graph.edges) edges.push_back(toJson(edge));
    return {{"task_id", graph.taskId}, {"nodes", nodes}, {"edges", edges}};
}

inline nlohmann::json toJson(const ProvenanceRecord &record) {
    return {{"event_id", record.eventId}, {"task_id", record.taskId}, {"run_id", record.runId}, {"agent", record.agent}, {"event_type", record.eventType}, {"timestamp", record.timestamp}, {"attribution", record.attribution}, {"changed_files", record.changedFiles}};
}

}
