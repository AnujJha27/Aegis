#pragma once

#include "daemon/agents/adapter.h"

#include <condition_variable>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>

namespace aegis::daemon::process { class ChildProcess; }

namespace aegis::daemon::agents {

class ClaudeAdapter final : public Adapter {
public:
    ClaudeAdapter(EventSink sink, SessionSink sessionSink);
    ~ClaudeAdapter() override;

    Capabilities capabilities() const override { return {true, false, true, true}; }
    bool start(const RunContext &context) override;
    SendResult send(std::string_view message) override;
    void interrupt() override;
    void terminate() override;
    bool hasActiveTurn() const override;
    std::vector<AgentEvent> pendingPermissions() const override;
    bool respondPermission(std::string_view requestId, bool allow) override;

private:
    void readLoop();
    void writerLoop();
    void handleLine(std::string_view line);
    void dispatchPending();
    void publish(std::string type, std::string content = "");

    RunContext context_;
    EventSink sink_;
    SessionSink sessionSink_;
    std::shared_ptr<process::ChildProcess> child_;
    std::thread reader_;
    std::thread writer_;
    std::condition_variable cv_;
    mutable std::mutex mutex_;
    bool running_ = false;
    bool stopping_ = false;
    bool initialized_ = false;
    bool busy_ = false;
    bool interrupted_ = false;
    std::string pendingPrompt_;
    std::map<std::string, AgentEvent> permissions_;
};

}
