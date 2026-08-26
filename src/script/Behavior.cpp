#include "script/Behavior.hpp"

#include <algorithm>
#include <utility>

namespace ege {

    Behavior::~Behavior() {
        // Ending them here rather than trusting onDespawn: a behaviour that
        // goes away for any other reason - a script reload rebuilding it, a
        // component detached, the world being torn down - would otherwise
        // leave the bus holding a call into freed memory.
        if (scene != nullptr) {
            for (const SubscriptionId subscription : subscriptions) {
                scene->events().unsubscribe(subscription);
            }
        }
    }

    TimerId Behavior::after(float seconds, std::function<void()> todo) {
        if (!todo) {
            return invalidTimer;
        }
        PendingTimer timer{};
        timer.id = nextTimer++;
        // A negative delay is a caller asking for "as soon as possible",
        // which is the next tick rather than an error.
        timer.remaining = std::max(seconds, 0.f);
        timer.todo = std::move(todo);
        const TimerId id = timer.id;
        timers.push_back(std::move(timer));
        return id;
    }

    void Behavior::cancel(TimerId timer) {
        const auto found =
            std::find_if(timers.begin(), timers.end(), [timer](const PendingTimer& pending) {
                return pending.id == timer;
            });
        if (found != timers.end()) {
            timers.erase(found);
        }
    }

    void Behavior::advanceTimers(float deltaSeconds) {
        if (timers.empty()) {
            return;
        }

        // Taken out of the list before any of them runs. A callback may set
        // another timer or cancel one, and a list being walked while it is
        // rewritten is the oldest bug in the callback business.
        std::vector<std::function<void()>> due;
        for (PendingTimer& timer : timers) {
            timer.remaining -= deltaSeconds;
            if (timer.remaining <= 0.f) {
                due.push_back(std::move(timer.todo));
                timer.id = invalidTimer;
            }
        }
        if (due.empty()) {
            return;
        }

        timers.erase(
            std::remove_if(
                timers.begin(),
                timers.end(),
                [](const PendingTimer& timer) { return timer.id == invalidTimer; }),
            timers.end());

        for (const std::function<void()>& todo : due) {
            todo();
        }
    }

}  // namespace ege
