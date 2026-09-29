#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace aegis::daemon {

struct Task {
    std::string id;
    std::string prompt;
    std::string repository;
    std::string status = "open";
    std::int64_t createdAt = 0;
};

struct AgentRun {
    std::string id;
    std::string taskId;
    std::string agent;
    std::string status = "starting";
    std::int64_t startedAt = 0;
    std::int64_t finishedAt = 0;
    std::optional<std::string> externalSessionId;
};

struct AgentEvent {
    std::string id;
    std::string taskId;
    std::string runId;
    std::string type;
    std::string agent;
    std::string content;
    std::int64_t timestamp = 0;
};

struct RepositoryState {
    std::string path;
    std::string branch;
    int files = 0;
    int insertions = 0;
    int deletions = 0;
};

struct GitChange {
    std::string path;
    std::optional<std::string> oldPath;
    std::string indexStatus;
    std::string worktreeStatus;
    int additions = 0;
    int deletions = 0;
    bool binary = false;
};

struct VerificationRun {
    std::string id;
    std::string taskId;
    std::optional<std::string> runId;
    std::vector<std::string> command;
    int exitCode = -1;
    std::string output;
    std::int64_t startedAt = 0;
    std::int64_t finishedAt = 0;
};

struct ReviewFinding {
    std::string id;
    std::string taskId;
    std::optional<std::string> runId;
    std::string filePath;
    std::optional<int> startLine;
    std::optional<int> endLine;
    std::string message;
    std::string status = "open";
    std::int64_t createdAt = 0;
    std::int64_t updatedAt = 0;
};

struct HandoffContext {
    std::string taskId;
    std::string prompt;
    std::vector<AgentEvent> recentEvents;
    std::string diff;
    std::vector<std::string> changedFiles;
    bool changedFilesTruncated = false;
    std::optional<VerificationRun> verification;
    std::vector<ReviewFinding> findings;
};

struct GraphNode {
    std::string id;
    std::string type;
    std::string label;
};

struct GraphEdge {
    std::string from;
    std::string to;
};

struct TaskGraph {
    std::string taskId;
    std::vector<GraphNode> nodes;
    std::vector<GraphEdge> edges;
};

struct ProvenanceRecord {
    std::string eventId;
    std::string taskId;
    std::string runId;
    std::string agent;
    std::string eventType;
    std::int64_t timestamp = 0;
    std::string attribution = "unknown";
    std::vector<std::string> changedFiles;
};

}
