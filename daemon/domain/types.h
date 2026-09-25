#pragma once

#include <cstdint>
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

struct VerificationRun {
    std::string id;
    std::string command;
    int exitCode = -1;
    std::string output;
    std::int64_t startedAt = 0;
    std::int64_t finishedAt = 0;
};

}
