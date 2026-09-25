#include "daemon/protocol/event_hub.h"

#include <algorithm>

namespace aegis::daemon {

EventHub::Subscription EventHub::subscribe() {
    auto subscription = std::make_shared<Queue>();
    std::lock_guard lock(subscribersMutex_);
    subscribers_.push_back(subscription);
    return subscription;
}

void EventHub::unsubscribe(const Subscription &subscription) {
    if (!subscription) return;
    {
        std::lock_guard lock(subscription->mutex);
        subscription->closed = true;
    }
    subscription->condition.notify_all();
}

void EventHub::publish(const AgentEvent &event) {
    std::lock_guard registryLock(subscribersMutex_);
    auto write = [&](const std::shared_ptr<Queue> &subscription) {
        std::lock_guard queueLock(subscription->mutex);
        if (subscription->closed) return;
        subscription->events.push_back(event);
        subscription->condition.notify_one();
    };
    std::erase_if(subscribers_, [&](const auto &weak) {
        const auto subscription = weak.lock();
        if (!subscription) return true;
        write(subscription);
        return false;
    });
}

bool EventHub::wait(const Subscription &subscription, AgentEvent &event, std::chrono::milliseconds timeout) {
    if (!subscription) return false;
    std::unique_lock lock(subscription->mutex);
    subscription->condition.wait_for(lock, timeout, [&] { return subscription->closed || !subscription->events.empty(); });
    if (subscription->events.empty()) return !subscription->closed;
    event = std::move(subscription->events.front());
    subscription->events.pop_front();
    return true;
}

}
