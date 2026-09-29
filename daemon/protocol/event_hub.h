#pragma once

#include "daemon/domain/types.h"

#include <condition_variable>
#include <chrono>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace aegis::daemon {

class EventHub final {
    struct Queue {
        std::mutex mutex;
        std::condition_variable condition;
        std::deque<AgentEvent> events;
        std::size_t queuedBytes = 0;
        bool closed = false;
        std::optional<std::string> runFilter;
    };

public:
    static constexpr std::size_t maxQueuedEvents = 512;
    static constexpr std::size_t maxQueuedBytes = 4 * 1024 * 1024;

    using Subscription = std::shared_ptr<Queue>;

    Subscription subscribe(std::optional<std::string> runFilter = std::nullopt);
    void unsubscribe(const Subscription &subscription);
    void publish(const AgentEvent &event);
    bool wait(const Subscription &subscription, AgentEvent &event, std::chrono::milliseconds timeout);

private:
    std::mutex subscribersMutex_;
    std::vector<std::weak_ptr<Queue>> subscribers_;
};

}
