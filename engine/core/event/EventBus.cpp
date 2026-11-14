// (c) SR VINIX
// SPDX-License-Identifier: MIT

#include "EventBus.h"

#include <algorithm>
#include <iterator>
#include <utility>

namespace quantum {

EventBus::SubscriptionId EventBus::subscribe(const std::string& type, Callback callback) {
    if (type.empty() || !callback) return 0;
    std::lock_guard<std::mutex> lock(mutex);
    const SubscriptionId id = nextId++;
    subscriptions.push_back(Subscription{id, type, std::move(callback)});
    return id;
}

bool EventBus::unsubscribe(SubscriptionId id) {
    if (id == 0) return false;
    std::lock_guard<std::mutex> lock(mutex);
    for (auto it = subscriptions.begin(); it != subscriptions.end(); ++it) {
        if (it->id == id) {
            subscriptions.erase(it);
            return true;
        }
    }
    return false;
}

void EventBus::publish(std::string type, std::vector<uint8_t> payload) {
    if (type.empty()) return;
    std::vector<Callback> callbacks;
    {
        std::lock_guard<std::mutex> lock(mutex);
        for (const Subscription& subscription : subscriptions) {
            if (subscription.type == type) callbacks.push_back(subscription.callback);
        }
    }
    const Event event{std::move(type), std::move(payload)};
    for (const Callback& callback : callbacks) callback(event);
}

void EventBus::publishQueued(std::string type, std::vector<uint8_t> payload) {
    if (type.empty()) return;
    std::lock_guard<std::mutex> lock(mutex);
    queuedEvents.push_back(Event{std::move(type), std::move(payload)});
}

size_t EventBus::dispatchQueued(size_t maxEvents) {
    std::vector<Event> events;
    {
        std::lock_guard<std::mutex> lock(mutex);
        const size_t count = maxEvents == 0 ? queuedEvents.size()
            : std::min(maxEvents, queuedEvents.size());
        events.reserve(count);
        events.insert(events.end(),
            std::make_move_iterator(queuedEvents.begin()),
            std::make_move_iterator(queuedEvents.begin() + count));
        queuedEvents.erase(queuedEvents.begin(), queuedEvents.begin() + count);
    }

    for (const Event& event : events) publish(event.type, event.payload);
    return events.size();
}

void EventBus::clear() {
    std::lock_guard<std::mutex> lock(mutex);
    subscriptions.clear();
    queuedEvents.clear();
}

EventBus& EventBus::global() {
    static EventBus bus;
    return bus;
}

}
