// The event bus: typed messages between things that have never met.
//
// Device-free and world-free - a bus is a table of callbacks - so all of it
// is exercised directly. What the tests are really about is the three rules
// that make immediate delivery safe, because those are what an author has to
// be able to rely on while a handler is running.

#include "scene/Events.hpp"

#include <doctest/doctest.h>

#include <string>
#include <vector>

using ege::EventBus;
using ege::invalidSubscription;
using ege::SubscriptionId;

namespace {

    struct Opened {
        int door = 0;
    };

    struct Closed {
        int door = 0;
    };

}  // namespace

TEST_CASE("a raised event reaches its subscribers and nobody else") {
    EventBus bus;
    std::vector<int> opened;
    std::vector<int> closed;

    bus.subscribe<Opened>([&](const Opened& event) { opened.push_back(event.door); });
    bus.subscribe<Closed>([&](const Closed& event) { closed.push_back(event.door); });

    bus.raise(Opened{7});

    CHECK(opened == std::vector<int>{7});
    // The type is the channel: a Closed subscriber hears nothing about an
    // Opened, and could not have been written to.
    CHECK(closed.empty());

    // An event nobody is listening for is not an error - the pickup does not
    // know whether anything is counting.
    bus.raise(Closed{1});
    CHECK(closed == std::vector<int>{1});
}

TEST_CASE("subscribers are called in the order they subscribed") {
    EventBus bus;
    std::string order;

    bus.subscribe<Opened>([&](const Opened&) { order += "a"; });
    bus.subscribe<Opened>([&](const Opened&) { order += "b"; });
    bus.subscribe<Opened>([&](const Opened&) { order += "c"; });

    bus.raise(Opened{});
    CHECK(order == "abc");
}

TEST_CASE("ending a subscription stops the calls") {
    EventBus bus;
    int calls = 0;
    const SubscriptionId id = bus.subscribe<Opened>([&](const Opened&) { calls++; });

    bus.raise(Opened{});
    CHECK(calls == 1);
    CHECK(bus.subscriberCount() == 1);

    bus.unsubscribe(id);
    bus.raise(Opened{});
    CHECK(calls == 1);
    CHECK(bus.subscriberCount() == 0);

    // Twice, and an id from nowhere: the caller's intent is already true.
    bus.unsubscribe(id);
    bus.unsubscribe(12345);
    bus.unsubscribe(invalidSubscription);
    CHECK(calls == 1);
}

TEST_CASE("a handler subscribed during a dispatch does not hear that dispatch") {
    EventBus bus;
    int latecomer = 0;

    bus.subscribe<Opened>(
        [&](const Opened&) { bus.subscribe<Opened>([&](const Opened&) { latecomer++; }); });

    // It was not listening when the thing happened.
    bus.raise(Opened{});
    CHECK(latecomer == 0);

    // It is listening now, and so is the one that added it - which adds
    // another.
    bus.raise(Opened{});
    CHECK(latecomer == 1);
}

TEST_CASE("a handler unsubscribed during a dispatch is not called by it") {
    EventBus bus;
    int second = 0;
    SubscriptionId later = invalidSubscription;

    bus.subscribe<Opened>([&](const Opened&) { bus.unsubscribe(later); });
    later = bus.subscribe<Opened>([&](const Opened&) { second++; });

    // Ending a subscription means ending it now, not after the event already
    // in flight - which is what lets a handler that despawns something stop
    // the rest of that thing's handlers from running.
    bus.raise(Opened{});
    CHECK(second == 0);
    CHECK(bus.subscriberCount() == 1);
}

TEST_CASE("a handler may raise, and a cycle is stopped rather than crashed") {
    EventBus bus;
    int opens = 0;
    int closes = 0;

    // The ordinary case: one event causing another, which is most of what a
    // bus is for.
    bus.subscribe<Opened>([&](const Opened&) {
        opens++;
        bus.raise(Closed{1});
    });
    bus.subscribe<Closed>([&](const Closed&) { closes++; });

    bus.raise(Opened{});
    CHECK(opens == 1);
    CHECK(closes == 1);

    // And the pathological one: a handler raising what it handles. It stops
    // at the depth limit and says so, rather than taking the stack with it.
    EventBus cycle;
    int depth = 0;
    cycle.subscribe<Opened>([&](const Opened&) {
        depth++;
        cycle.raise(Opened{});
    });
    cycle.raise(Opened{});
    CHECK(depth == EventBus::maxDispatchDepth);
}

TEST_CASE("clearing ends every subscription") {
    EventBus bus;
    int calls = 0;
    bus.subscribe<Opened>([&](const Opened&) { calls++; });
    bus.subscribe<Closed>([&](const Closed&) { calls++; });
    CHECK(bus.subscriberCount() == 2);

    bus.clear();
    CHECK(bus.subscriberCount() == 0);

    bus.raise(Opened{});
    bus.raise(Closed{});
    CHECK(calls == 0);
}

TEST_CASE("an empty handler is refused rather than stored") {
    EventBus bus;
    CHECK(bus.subscribe<Opened>({}) == invalidSubscription);
    CHECK(bus.subscriberCount() == 0);
    // And raising still works, which is the point of checking.
    bus.raise(Opened{});
}
