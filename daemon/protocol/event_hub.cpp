#include "daemon/protocol/event_hub.h"

#include <algorithm>

namespace aegis::daemon {
namespace {

std::size_t eventBytes(const AgentEvent &event) {
    return 128 + event.id.size() + event.taskId.size() + event.runId.size() +
        event.type.size() + event.agent.size() + event.content.size();
}

}

EventHub::Subscription EventHub::subscribe(std::optional<std::string> runFilter) {
    auto subscription = std::make_shared<Queue>();
    subscription->runFilter = std::move(runFilter);
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
        if (subscription->closed || (subscription->runFilter && *subscription->runFilter != event.runId)) return;
        const auto bytes = eventBytes(event);
        if (subscription->events.size() >= maxQueuedEvents || bytes > maxQueuedBytes - subscription->queuedBytes) {
            AgentEvent marker{event.id + "-resync", event.taskId, event.runId, "stream.resync_required", event.agent,
                "The live event backlog exceeded its memory limit. Reconnecting will replay persisted history.", event.timestamp};
            const auto markerBytes = eventBytes(marker);
            while (!subscription->events.empty() &&
                   (subscription->events.size() >= maxQueuedEvents ||
                    markerBytes > maxQueuedBytes - subscription->queuedBytes)) {
                subscription->queuedBytes -= eventBytes(subscription->events.back());
                subscription->events.pop_back();
            }
            subscription->events.push_back(std::move(marker));
            subscription->queuedBytes += markerBytes;
            subscription->highWaterEvents = std::max(subscription->highWaterEvents, subscription->events.size());
            subscription->highWaterBytes = std::max(subscription->highWaterBytes, subscription->queuedBytes);
            subscription->closed = true;
            subscription->condition.notify_one();
            return;
        }
        subscription->events.push_back(event);
        subscription->queuedBytes += bytes;
        subscription->highWaterEvents = std::max(subscription->highWaterEvents, subscription->events.size());
        subscription->highWaterBytes = std::max(subscription->highWaterBytes, subscription->queuedBytes);
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
    if (!subscription->condition.wait_for(lock, timeout, [&] { return subscription->closed || !subscription->events.empty(); }) || subscription->events.empty()) return false;
    event = std::move(subscription->events.front());
    subscription->queuedBytes -= eventBytes(event);
    subscription->events.pop_front();
    return true;
}

EventHub::HighWater EventHub::highWater(const Subscription &subscription) const {
    if (!subscription) return {};
    std::lock_guard lock(subscription->mutex);
    return {subscription->highWaterEvents, subscription->highWaterBytes};
}

}
