#pragma once

#include "daemon/agents/adapter.h"
#include "daemon/domain/types.h"
#include "daemon/protocol/event_hub.h"
#include "daemon/session/store.h"

#include <filesystem>
#include <memory>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace aegis::daemon::agents {

struct AgentInfo {
    std::string name;
    bool available = false;
    Capabilities capabilities;
};

class Manager final {
public:
    Manager(std::filesystem::path repository, Store &store, EventHub &events);
    ~Manager();

    std::vector<AgentInfo> available() const;
    bool hasRunningRuns() const;
    std::optional<AgentRun> launch(const std::string &taskId, const std::string &agent);
    bool send(const std::string &runId, std::string_view message);
    bool sendPty(const std::string &runId, std::string_view input);
    bool interrupt(const std::string &runId);
    bool terminate(const std::string &runId);

private:
    void publish(AgentEvent event);

    std::filesystem::path repository_;
    Store &store_;
    EventHub &events_;
    mutable std::mutex mutex_;
    std::map<std::string, std::unique_ptr<Adapter>> active_;
    mutable std::mutex provenanceMutex_;
    std::set<std::string> running_;
    std::map<std::string, std::map<std::string, std::string>> initialGitStatus_;
};

}
