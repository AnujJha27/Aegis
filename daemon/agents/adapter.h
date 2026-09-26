#pragma once

#include "daemon/domain/types.h"

#include <filesystem>
#include <functional>
#include <string>
#include <string_view>

namespace aegis::daemon::agents {

struct Capabilities {
    bool structured = false;
    bool interactive = false;
};

struct RunContext {
    std::string taskId;
    std::string runId;
    std::string agent;
    std::filesystem::path repository;
};

using EventSink = std::function<void(AgentEvent)>;

class Adapter {
public:
    virtual ~Adapter() = default;
    virtual Capabilities capabilities() const = 0;
    virtual bool start(const RunContext &context) = 0;
    virtual void send(std::string_view message) = 0;
    virtual bool sendPty(std::string_view) { return false; }
    virtual void interrupt() = 0;
    virtual void terminate() = 0;
};

}
