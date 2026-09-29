#pragma once

#include "daemon/domain/types.h"
#include "daemon/repository/files.h"

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
    return {{"path", change.path}, {"old_path", change.oldPath ? nlohmann::json(*change.oldPath) : nlohmann::json(nullptr)},
            {"index_status", change.indexStatus}, {"worktree_status", change.worktreeStatus},
            {"additions", change.additions}, {"deletions", change.deletions}, {"binary", change.binary}};
}

inline nlohmann::json toJson(const repository::FileEntry &entry) {
    return {{"path", entry.path}, {"name", entry.name}, {"kind", entry.kind}, {"language", entry.language},
            {"size", entry.size}, {"changed", entry.changed}, {"git_status", entry.gitStatus},
            {"old_path", entry.oldPath ? nlohmann::json(*entry.oldPath) : nlohmann::json(nullptr)},
            {"additions", entry.additions}, {"deletions", entry.deletions}, {"binary", entry.binary}};
}

inline nlohmann::json toJson(const repository::FileContent &content) {
    return {{"source", repository::fileSourceName(content.source)},
            {"revision", content.revision ? nlohmann::json(*content.revision) : nlohmann::json(nullptr)},
            {"size", content.size}, {"exists", content.exists},
            {"binary", content.binary}, {"truncated", content.truncated}, {"content", content.content}};
}

inline nlohmann::json toJson(const repository::FileComparison &comparison) {
    return {{"path", comparison.path}, {"old_path", comparison.oldPath ? nlohmann::json(*comparison.oldPath) : nlohmann::json(nullptr)},
            {"parent_commit", comparison.parentCommit ? nlohmann::json(*comparison.parentCommit) : nlohmann::json(nullptr)},
            {"commit", comparison.commit ? nlohmann::json(*comparison.commit) : nlohmann::json(nullptr)},
            {"status", comparison.status}, {"original", toJson(comparison.original)}, {"modified", toJson(comparison.modified)},
            {"binary", comparison.binary}, {"truncated", comparison.truncated}};
}

inline nlohmann::json toJson(const repository::CommitSummary &commit) {
    return {{"id", commit.id}, {"parent_id", commit.parentId ? nlohmann::json(*commit.parentId) : nlohmann::json(nullptr)},
            {"author", commit.author}, {"timestamp", commit.timestamp}, {"subject", commit.subject}};
}

inline nlohmann::json toJson(const repository::CommitFile &file) {
    return {{"path", file.path}, {"old_path", file.oldPath ? nlohmann::json(*file.oldPath) : nlohmann::json(nullptr)},
            {"status", file.status}};
}

inline nlohmann::json toJson(const VerificationRun &run) {
    return {{"id", run.id}, {"task_id", run.taskId}, {"run_id", run.runId ? nlohmann::json(*run.runId) : nlohmann::json(nullptr)}, {"command", run.command}, {"exit_code", run.exitCode}, {"output", run.output}, {"started_at", run.startedAt}, {"finished_at", run.finishedAt}};
}

inline nlohmann::json toJson(const ReviewFinding &finding) {
    return {{"id", finding.id}, {"task_id", finding.taskId},
            {"run_id", finding.runId ? nlohmann::json(*finding.runId) : nlohmann::json(nullptr)},
            {"file_path", finding.filePath}, {"start_line", finding.startLine ? nlohmann::json(*finding.startLine) : nlohmann::json(nullptr)},
            {"end_line", finding.endLine ? nlohmann::json(*finding.endLine) : nlohmann::json(nullptr)},
            {"message", finding.message}, {"status", finding.status},
            {"created_at", finding.createdAt}, {"updated_at", finding.updatedAt}};
}

inline nlohmann::json toJson(const HandoffContext &context) {
    nlohmann::json events = nlohmann::json::array();
    for (const auto &event : context.recentEvents) events.push_back(toJson(event));
    nlohmann::json findings = nlohmann::json::array();
    for (const auto &finding : context.findings) findings.push_back(toJson(finding));
    nlohmann::json result{{"task_id", context.taskId}, {"prompt", context.prompt}, {"recent_events", events}, {"diff", context.diff}, {"changed_files", context.changedFiles}, {"changed_files_truncated", context.changedFilesTruncated}};
    result["verification"] = context.verification ? toJson(*context.verification) : nlohmann::json(nullptr);
    result["findings"] = findings;
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
