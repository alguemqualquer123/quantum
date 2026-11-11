// (c) SR VINIX
// SPDX-License-Identifier: MIT

#ifndef QUANTUM_EVENT_BUS_H
#define QUANTUM_EVENT_BUS_H

#include "../Export.h"

#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

namespace quantum {

// Thread-safe process event bus. Publishers never execute user callbacks while
// holding the bus lock, so a callback may safely unsubscribe or publish again.
class QUANTUM_API EventBus {
public:
    struct Event {
        std::string type;
        std::vector<uint8_t> payload;
    };

    using SubscriptionId = uint64_t;
    using Callback = std::function<void(const Event&)>;

    SubscriptionId subscribe(const std::string& type, Callback callback);
    bool unsubscribe(SubscriptionId id);
    void publish(std::string type, std::vector<uint8_t> payload = {});
    void publishQueued(std::string type, std::vector<uint8_t> payload = {});
    size_t dispatchQueued(size_t maxEvents = 0);
    void clear();

    static EventBus& global();

private:
    struct Subscription {
        SubscriptionId id = 0;
        std::string type;
        Callback callback;
    };

    std::mutex mutex;
    std::vector<Subscription> subscriptions;
    std::vector<Event> queuedEvents;
    SubscriptionId nextId = 1;
};

}

#endif // QUANTUM_EVENT_BUS_H
