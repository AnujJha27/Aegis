#pragma once

#include "daemon/agents/adapter.h"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <mutex>
#include <thread>
#include <vector>

namespace aegis::daemon::agents {

struct PtyOutputBatching {
    std::chrono::milliseconds flushInterval{16};
    std::size_t maxBatchBytes = 64 * 1024;
};

class PtyAdapter final : public Adapter {
public:
    PtyAdapter(std::string name, std::vector<std::string> command, EventSink sink,
               PtyOutputBatching batching = {});
    ~PtyAdapter() override;

    Capabilities capabilities() const override { return {false, true, true, true}; }
    bool start(const RunContext &context) override;
    SendResult send(std::string_view message) override;
    bool sendPty(std::string_view input) override;
    bool resizePty(unsigned short cols, unsigned short rows) override;
    void interrupt() override;
    void terminate() override;

private:
    void readLoop(int master, int child);
    void publish(std::string type, std::string content);

    std::string name_;
    std::vector<std::string> command_;
    EventSink sink_;
    PtyOutputBatching batching_;
    RunContext context_;
    std::atomic_bool running_ = false;
    int master_ = -1;
    int pid_ = -1;
    std::thread reader_;
    std::mutex writeMutex_;
};

}
