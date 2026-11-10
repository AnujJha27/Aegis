#pragma once

#include "daemon/domain/types.h"

#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

struct sqlite3;

namespace aegis::daemon {

class Store final {
public:
    static constexpr int currentSchemaVersion = 14;

    explicit Store(const std::filesystem::path &path);
    ~Store();

    Store(const Store &) = delete;
    Store &operator=(const Store &) = delete;

    Task createTask(std::string prompt, std::string repository);
    Task importTask(const std::string &sourceKey, std::string prompt, std::string repository,
                    const TaskContext &context, const std::vector<ImportedMessage> &messages);
    AgentRun startRun(const std::string &taskId, std::string agent, const std::optional<std::string> &model = std::nullopt);
    bool updateRunStatus(const std::string &runId, const std::string &status);
    bool setExternalSessionId(const std::string &runId, std::string sessionId);
    std::optional<AgentRun> run(const std::string &runId) const;
    bool deleteRun(const std::string &runId);
    void appendEvent(const AgentEvent &event);
    void saveVerification(const VerificationRun &verification);
    ReviewFinding createFinding(const std::string &taskId, std::optional<std::string> runId,
                                std::string filePath, std::optional<int> startLine,
                                std::optional<int> endLine, std::string message,
                                std::optional<std::string> fingerprint = std::nullopt);
    bool updateFindingStatus(const std::string &findingId, const std::string &status);
    std::optional<Task> updateTaskContext(const std::string &taskId, const TaskContext &context,
                                          std::optional<std::int64_t> expectedRevision = std::nullopt);
    std::optional<Task> task(const std::string &taskId) const;
    std::vector<AgentRun> runs(const std::string &taskId) const;
    std::optional<AgentEvent> latestTurnEvent(const std::string &runId) const;
    std::vector<Task> tasks() const;
    std::vector<AgentEvent> events(const std::string &taskId, std::optional<std::size_t> limit = std::nullopt,
                                   bool includeTerminalOutput = true) const;
    AgentEventPage eventsBefore(const std::string &taskId, std::int64_t sequence, std::size_t limit = 100) const;
    std::vector<AgentEvent> terminalOutput(const std::string &runId, std::size_t limit = 128) const;
    std::vector<VerificationRun> verifications(const std::string &taskId, std::size_t limit = 20) const;
    std::vector<ReviewFinding> findings(const std::string &taskId, std::size_t limit = 100) const;
    bool reserveContinuation(const std::string &requestId, const std::string &payload);
    std::optional<std::pair<std::string, std::optional<std::string>>> continuation(const std::string &requestId) const;
    bool completeContinuation(const std::string &requestId, const std::string &runId);
    std::optional<std::string> continuationState(const std::string &requestId) const;
    bool linkContinuation(const std::string &requestId, const std::string &runId);
    bool failContinuation(const std::string &requestId);
    std::string exportTaskBundle(const std::string &taskId) const;
    Task importTaskBundle(const std::string &raw, const std::string &destRepository);
    std::pair<std::string, std::int64_t> workingProfile(const std::string &repository) const;
    std::optional<std::int64_t> saveWorkingProfile(const std::string &repository,
                                                   const std::string &profileJson, std::int64_t expectedRevision);

private:
    void execute(const char *sql) const;

    sqlite3 *database_ = nullptr;
    mutable std::mutex mutex_;
};

}
