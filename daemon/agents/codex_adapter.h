#pragma once

#include "daemon/agents/adapter.h"

#include <atomic>
#include <optional>
#include <thread>

namespace aegis::daemon::agents {

std::optional<AgentEvent> parseCodexJsonLine(std::string_view line,
                                             const std::string &taskId,
                                             const std::string &runId,
                                             const std::string &agent);

class CodexAdapter final : public Adapter {
public:
    explicit CodexAdapter(EventSink sink);
    ~CodexAdapter() override;

    Capabilities capabilities() const override { return {true, false}; }
    bool start(const RunContext &context) override;
    void send(std::string_view message) override;
    void interrupt() override;
    void terminate() override;

private:
    EventSink sink_;
    RunContext context_;
    std::atomic_bool running_ = false;
    std::thread worker_;
};

}
