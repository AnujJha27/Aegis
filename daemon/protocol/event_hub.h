#pragma once

#include "daemon/domain/types.h"

#include <condition_variable>
#include <chrono>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <vector>

namespace aegis::daemon {

class EventHub final {
    struct Queue {
        std::mutex mutex;
        std::condition_variable condition;
        std::deque<AgentEvent> events;
        bool closed = false;
    };

public:
    using Subscription = std::shared_ptr<Queue>;

    Subscription subscribe();
    void unsubscribe(const Subscription &subscription);
    void publish(const AgentEvent &event);
    bool wait(const Subscription &subscription, AgentEvent &event, std::chrono::milliseconds timeout);

private:
    std::mutex subscribersMutex_;
    std::vector<std::weak_ptr<Queue>> subscribers_;
};

}
