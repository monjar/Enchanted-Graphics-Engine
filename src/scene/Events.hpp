#pragma once

#include "core/Log.hpp"

#include <cstdint>
#include <functional>
#include <memory>
#include <typeindex>
#include <unordered_map>
#include <vector>

namespace ege {

    // What a subscription is, to whoever wants to end it. Zero is nobody.
    using SubscriptionId = std::uint64_t;

    inline constexpr SubscriptionId invalidSubscription = 0;

    // Typed messages between things that do not know about each other.
    //
    // The alternative gameplay reaches for first is a pointer: the pickup
    // finds the scoreboard and calls a method on it. That works until there
    // are two scoreboards, or none, or the pickup is spawned from a prefab
    // that cannot know what scene it landed in. An event is the same message
    // with the introduction removed - the pickup says what happened, and
    // whoever cares was already listening.
    //
    // An event is any type at all. There is no registration, no base class
    // and no reflection: the type itself is the channel, so a subscriber that
    // asks for the wrong one does not compile rather than not firing.
    //
    // Delivery is immediate and in subscription order, because a queue would
    // buy re-entrancy safety at the price of "when does this happen" being a
    // thing the author has to look up. The three rules that make immediacy
    // safe are worth stating:
    //
    //  - A handler subscribed *during* a dispatch does not receive the event
    //    being dispatched. It was not listening when the thing happened.
    //  - A handler unsubscribed during a dispatch is not called, even if the
    //    dispatch had not reached it yet. Ending a subscription means ending
    //    it now.
    //  - A handler may raise events, including of the type it is handling,
    //    down to a depth after which the bus stops and says so. A cycle
    //    should be a log line rather than a stack overflow.
    class EventBus {
    public:
        // How deep raise-inside-a-handler may nest before the bus refuses.
        // Sixteen is far past any honest chain and far short of a stack.
        static constexpr int maxDispatchDepth = 16;

        // Calls `handler` whenever an Event is raised, until the returned
        // subscription is ended.
        template<typename Event>
        SubscriptionId subscribe(std::function<void(const Event&)> handler);

        template<typename Event>
        void raise(const Event& event);

        // Ends one subscription. An id that is not subscribed - already
        // ended, or from a bus that has been cleared - is a no-op, because
        // the caller's intent is already true.
        void unsubscribe(SubscriptionId subscription);

        // Everything, for Stop: play is over and the listeners were play's.
        void clear();

        std::size_t subscriberCount() const;

    private:
        struct Handler {
            SubscriptionId id = invalidSubscription;
            std::function<void(const void*)> call;
            // Ended, but not yet removed: a dispatch may be walking the list
            // that holds it.
            bool alive = true;
        };

        // Handlers are held by pointer so that subscribing from inside a
        // dispatch - which grows this vector - cannot move the handler
        // currently being called out from under itself.
        struct Channel {
            std::vector<std::unique_ptr<Handler>> handlers;
            int dispatching = 0;
        };

        void compact(Channel& channel);

        std::unordered_map<std::type_index, Channel> channels;
        // Which channel an id is in, so ending a subscription does not search
        // every one of them.
        std::unordered_map<SubscriptionId, std::type_index> owners;
        SubscriptionId nextId = 1;
    };

    template<typename Event>
    SubscriptionId EventBus::subscribe(std::function<void(const Event&)> handler) {
        if (!handler) {
            return invalidSubscription;
        }
        const std::type_index key{typeid(Event)};
        Channel& channel = channels.try_emplace(key).first->second;

        auto entry = std::make_unique<Handler>();
        entry->id = nextId++;
        entry->call = [handler = std::move(handler)](const void* event) {
            handler(*static_cast<const Event*>(event));
        };
        const SubscriptionId id = entry->id;
        channel.handlers.push_back(std::move(entry));
        owners.emplace(id, key);
        return id;
    }

    template<typename Event>
    void EventBus::raise(const Event& event) {
        const auto found = channels.find(std::type_index{typeid(Event)});
        if (found == channels.end()) {
            return;
        }
        Channel& channel = found->second;
        if (channel.dispatching >= maxDispatchDepth) {
            EGE_ERROR(
                "event dispatch is {} deep; a handler is raising what it handles",
                maxDispatchDepth);
            return;
        }

        // The size now, so that a handler subscribing during this dispatch is
        // not called by it.
        const std::size_t count = channel.handlers.size();
        channel.dispatching++;
        for (std::size_t index = 0; index < count; index++) {
            // Re-read each time: the vector may have grown, which moves the
            // pointers but not what they point at.
            const Handler& handler = *channel.handlers[index];
            if (handler.alive) {
                handler.call(&event);
            }
        }
        channel.dispatching--;
        if (channel.dispatching == 0) {
            compact(channel);
        }
    }

}  // namespace ege
