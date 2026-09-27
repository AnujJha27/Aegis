#pragma once

#include "daemon/domain/types.h"

#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

namespace aegis::daemon::agents {

struct Capabilities {
    bool structured = false;
    bool interactive = false;
    bool resumable = false;
    bool interruptible = false;
};

enum class SendResult { accepted, busy, unavailable };

struct RunContext {
    std::string taskId;
    std::string runId;
    std::string agent;
    std::filesystem::path repository;
    std::optional<std::string> externalSessionId;
};

using EventSink = std::function<void(AgentEvent)>;
using SessionSink = std::function<void(const std::string &, std::string)>;

class Adapter {
public:
    virtual ~Adapter() = default;
    virtual Capabilities capabilities() const = 0;
    virtual bool start(const RunContext &context) = 0;
    virtual SendResult send(std::string_view message) = 0;
    virtual bool sendPty(std::string_view) { return false; }
    virtual bool resizePty(unsigned short, unsigned short) { return false; }
    virtual void interrupt() = 0;
    virtual void terminate() = 0;
};

}
