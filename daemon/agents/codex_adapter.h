#pragma once

#include "daemon/agents/adapter.h"

#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <vector>

namespace aegis::daemon::process { class ChildProcess; }

namespace aegis::daemon::agents {

std::optional<AgentEvent> parseCodexJsonLine(std::string_view line,
                                             const std::string &taskId,
                                             const std::string &runId,
                                             const std::string &agent);
std::vector<AgentEvent> parseCodexJsonOutput(std::string_view output,
                                             const std::string &taskId,
                                             const std::string &runId,
                                             const std::string &agent);

class CodexAdapter final : public Adapter {
public:
    CodexAdapter(EventSink sink, SessionSink sessionSink);
    ~CodexAdapter() override;

    Capabilities capabilities() const override { return {true, false, true, true}; }
    bool start(const RunContext &context) override;
    SendResult send(std::string_view message) override;
    void interrupt() override;
    void terminate() override;

private:
    void workerLoop();
    void publish(AgentEvent event);

    EventSink sink_;
    SessionSink sessionSink_;
    RunContext context_;
    std::atomic_bool running_ = false;
    std::atomic_bool busy_ = false;
    std::mutex mutex_;
    std::condition_variable condition_;
    std::string pendingPrompt_;
    bool hasPendingPrompt_ = false;
    bool stopping_ = false;
    bool interrupted_ = false;
    std::shared_ptr<process::ChildProcess> child_;
    std::thread worker_;
};

}
