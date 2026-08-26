#include "scene/Events.hpp"

#include <algorithm>

namespace ege {

    void EventBus::unsubscribe(SubscriptionId subscription) {
        const auto owner = owners.find(subscription);
        if (owner == owners.end()) {
            return;
        }
        const auto found = channels.find(owner->second);
        owners.erase(owner);
        if (found == channels.end()) {
            return;
        }

        Channel& channel = found->second;
        for (std::unique_ptr<Handler>& handler : channel.handlers) {
            if (handler->id == subscription) {
                // Marked rather than removed, because a dispatch may be
                // walking this list right now - and marked *before* that
                // dispatch reaches it, which is what makes ending a
                // subscription take effect immediately rather than next time.
                handler->alive = false;
                break;
            }
        }
        if (channel.dispatching == 0) {
            compact(channel);
        }
    }

    void EventBus::compact(Channel& channel) {
        channel.handlers.erase(
            std::remove_if(
                channel.handlers.begin(),
                channel.handlers.end(),
                [](const std::unique_ptr<Handler>& handler) { return !handler->alive; }),
            channel.handlers.end());
    }

    void EventBus::clear() {
        // Not while dispatching: a handler that cleared the bus would be
        // deleting the list its own caller is walking. Nothing in the engine
        // does this, and saying so is cheaper than the crash.
        for (auto& [key, channel] : channels) {
            if (channel.dispatching > 0) {
                EGE_ERROR("the event bus was cleared from inside a handler; ignored");
                return;
            }
        }
        channels.clear();
        owners.clear();
    }

    std::size_t EventBus::subscriberCount() const {
        std::size_t total = 0;
        for (const auto& [key, channel] : channels) {
            for (const std::unique_ptr<Handler>& handler : channel.handlers) {
                total += handler->alive ? std::size_t{1} : std::size_t{0};
            }
        }
        return total;
    }

}  // namespace ege
