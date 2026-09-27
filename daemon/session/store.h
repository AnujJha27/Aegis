#pragma once

#include "daemon/domain/types.h"

#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

struct sqlite3;

namespace aegis::daemon {

class Store final {
public:
    static constexpr int currentSchemaVersion = 2;

    explicit Store(const std::filesystem::path &path);
    ~Store();

    Store(const Store &) = delete;
    Store &operator=(const Store &) = delete;

    Task createTask(std::string prompt, std::string repository);
    AgentRun startRun(const std::string &taskId, std::string agent);
    bool updateRunStatus(const std::string &runId, const std::string &status);
    bool setExternalSessionId(const std::string &runId, std::string sessionId);
    std::optional<AgentRun> run(const std::string &runId) const;
    bool deleteRun(const std::string &runId);
    void appendEvent(const AgentEvent &event);
    void saveVerification(const VerificationRun &verification);
    ReviewFinding createFinding(const std::string &taskId, std::optional<std::string> runId,
                                std::string filePath, std::optional<int> startLine,
                                std::optional<int> endLine, std::string message);
    bool updateFindingStatus(const std::string &findingId, const std::string &status);
    std::optional<Task> task(const std::string &taskId) const;
    std::vector<AgentRun> runs(const std::string &taskId) const;
    std::vector<Task> tasks() const;
    std::vector<AgentEvent> events(const std::string &taskId) const;
    std::vector<VerificationRun> verifications(const std::string &taskId, std::size_t limit = 20) const;
    std::vector<ReviewFinding> findings(const std::string &taskId, std::size_t limit = 100) const;

private:
    void execute(const char *sql) const;

    sqlite3 *database_ = nullptr;
    mutable std::mutex mutex_;
};

}
