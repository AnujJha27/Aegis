#pragma once

#include "daemon/domain/types.h"

#include <filesystem>
#include <string>
#include <vector>

struct sqlite3;

namespace aegis::daemon {

class Store final {
public:
    explicit Store(const std::filesystem::path &path);
    ~Store();

    Store(const Store &) = delete;
    Store &operator=(const Store &) = delete;

    Task createTask(std::string prompt, std::string repository);
    AgentRun startRun(const std::string &taskId, std::string agent);
    void appendEvent(const AgentEvent &event);
    std::vector<Task> tasks() const;
    std::vector<AgentEvent> events(const std::string &taskId) const;

private:
    void execute(const char *sql) const;

    sqlite3 *database_ = nullptr;
};

}
