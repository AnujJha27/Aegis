#pragma once

#include "daemon/agents/adapter.h"

#include <atomic>
#include <mutex>
#include <thread>
#include <vector>

namespace aegis::daemon::agents {

class PtyAdapter final : public Adapter {
public:
    PtyAdapter(std::string name, std::vector<std::string> command, EventSink sink);
    ~PtyAdapter() override;

    Capabilities capabilities() const override { return {false, true}; }
    bool start(const RunContext &context) override;
    void send(std::string_view message) override;
    bool sendPty(std::string_view input) override;
    void interrupt() override;
    void terminate() override;

private:
    void readLoop();
    void publish(std::string type, std::string content);

    std::string name_;
    std::vector<std::string> command_;
    EventSink sink_;
    RunContext context_;
    std::atomic_bool running_ = false;
    int master_ = -1;
    int pid_ = -1;
    std::thread reader_;
    std::mutex writeMutex_;
};

}
