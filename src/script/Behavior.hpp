#pragma once

#include "scene/World.hpp"

#include <glm/glm.hpp>

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace ege {

    // A touch that began this fixed step, as one entity experiences it: the
    // physics system reports each new contact pair once, and the script
    // system hands each side its own view of it.
    struct Contact {
        Entity other{};
        // Where the bodies met, in world space.
        glm::vec3 point{0.f};
        // From this entity towards the other.
        glm::vec3 normal{0.f};
    };

    // Gameplay code attached to an entity.
    //
    // Virtual calls rather than the ECS's data-oriented iteration, on purpose.
    // Systems are how the engine expresses work that applies to *every* entity
    // with a component; behaviours are how a project expresses work that
    // applies to *this* entity and reads like the thing it is doing. The cost
    // is one indirect call per behaviour per tick, which is nothing against a
    // scene's worth of draws, and the benefit is that gameplay written against
    // this looks like gameplay rather than like a query.
    //
    // The callbacks are deliberately few. Every one of them is a promise about
    // when it runs, and a promise is easier to keep than to withdraw.
    // What a pending timer is, to whoever wants to cancel it. Zero is nobody.
    using TimerId = std::uint64_t;

    inline constexpr TimerId invalidTimer = 0;

    class Behavior {
    public:
        // Ends this behaviour's subscriptions. Pending timers go with the
        // object, since they are the object's.
        virtual ~Behavior();

        Behavior(const Behavior&) = delete;
        Behavior& operator=(const Behavior&) = delete;

        // Once, when the entity starts playing - not when the component is
        // attached. Attaching happens in the editor, where nothing should run.
        virtual void onSpawn() {}

        // Every rendered frame, with the real elapsed time. For anything that
        // should look smooth.
        virtual void onTick(float deltaSeconds) { (void)deltaSeconds; }

        // Every fixed simulation step. For anything that must be
        // frame-rate-independent - which, now that physics exists, is most
        // of it.
        virtual void onFixedTick(float deltaSeconds) { (void)deltaSeconds; }

        // When this entity's body begins touching another, after the fixed
        // step that detected it. Both entities are told, each from its own
        // side. Requires a collider, since without one there is no body to
        // be touched.
        virtual void onContact(const Contact& contact) { (void)contact; }

        // When something arrives in this entity's Trigger volume, and when
        // the last of it leaves. Both sides hear both calls: a behaviour on
        // the plate is told who stepped on it, and a behaviour on whoever
        // stepped is told which plate - so either end can be the one that
        // knows what to do.
        //
        // `other` may be dead by the time a leaving is reported, because
        // being despawned inside a volume is one of the ways to leave it -
        // that is what a pickup is - so check before reaching through it.
        virtual void onTriggerEnter(Entity other) { (void)other; }

        virtual void onTriggerExit(Entity other) { (void)other; }

        // When play stops, or the entity is despawned while playing.
        virtual void onDespawn() {}

        // ---- Surviving a script reload ------------------------------------

        // What this behaviour wants to keep across a reload, beyond the
        // reflected fields that always cross.
        //
        // Called on the instance being *replaced*, just before it goes, and
        // handed to the replacement's onReload. Empty by default, which is
        // what a reload did before this existed.
        //
        // Text rather than the object itself, and that is the whole design.
        // A reload happens because the source file changed: the replacement's
        // idea of the class layout is not the old one's, so reaching into the
        // old object through the new type is reading a struct that may have
        // grown a member since. The only thing that can safely cross a reload
        // is data the outgoing instance describes using its own code - which
        // is what this call is. Any format will do; the author writes it and
        // the author reads it.
        virtual std::string onSaveState() { return {}; }

        // Called on the replacement *instead of* onSpawn, when the behaviour
        // it replaces was already playing. `state` is whatever the outgoing
        // instance's onSaveState returned.
        //
        // The default calls onSpawn, so a behaviour that says nothing about
        // reloading lands exactly where a fresh Play would - which is the
        // rule the reload documentation has always given. An override that
        // wants both should say so:
        //
        //     void onReload(const std::string& state) override {
        //         onSpawn();                 // subscriptions, timers, setup
        //         time = std::stof(state);   // and the thing worth keeping
        //     }
        //
        // What does not cross either way: the outgoing instance's pending
        // timers and its event subscriptions, both of which end with it. That
        // is why an override that skips onSpawn stops hearing events - the
        // subscription belonged to the object that just went.
        virtual void onReload(const std::string& state) {
            (void)state;
            onSpawn();
        }

        // The entity this is attached to. Valid from onSpawn onwards, and the
        // entity handle carries its world, so `self().fetch<Transform>()` is
        // the whole of the usual first line of a behaviour.
        Entity self() const { return owner; }

        World& world() const { return *scene; }

    protected:
        Behavior() = default;

        // ---- Timers --------------------------------------------------------

        // Runs `todo` once, `seconds` of simulated time from now.
        //
        // Simulated, not real: timers advance on the fixed tick, so a timer
        // is as frame-rate-independent as the physics beside it and a
        // recorded run reproduces exactly. A paused game's timers do not
        // advance, because nothing is ticking them - which is the behaviour
        // everybody wants and nobody has to ask for.
        //
        // The callback belongs to this behaviour and dies with it: an entity
        // despawned with a timer pending is an entity whose timer never
        // fires, which is what stops a door opening for something that is no
        // longer there. Cancel it early with the returned handle.
        //
        // Timers are advanced *before* onFixedTick, so one set during a tick
        // gets its whole duration before it is first looked at, and
        // `after(0.f, ...)` means "next tick" rather than "later in this
        // one".
        TimerId after(float seconds, std::function<void()> todo);

        // Ends a timer that has not fired. An id that has already fired, or
        // was already cancelled, is a no-op.
        void cancel(TimerId timer);

        // ---- Events --------------------------------------------------------

        // Listens for an event on the scene's bus until this behaviour goes.
        //
        // The tidying is the point: a subscription outliving its subscriber
        // is a call into a destroyed object, and the shape that prevents it -
        // remember every token, end them all in the destructor - is one every
        // subscriber would otherwise have to write correctly. Subscribe from
        // onSpawn; a reload rebuilds the instance and calls onSpawn again, so
        // a behaviour that subscribes there keeps listening across one.
        template<typename Event>
        void on(std::function<void(const Event&)> handler) {
            if (scene == nullptr) {
                return;
            }
            const SubscriptionId id = scene->events().subscribe<Event>(std::move(handler));
            if (id != invalidSubscription) {
                subscriptions.push_back(id);
            }
        }

        // Says that something happened. Every listener hears it before this
        // returns - see EventBus for why immediacy is the choice.
        template<typename Event>
        void raise(const Event& event) {
            if (scene != nullptr) {
                scene->events().raise(event);
            }
        }

    private:
        friend class ScriptSystem;

        // Ticks pending timers and runs the ones that came due. Called by the
        // script system rather than by an author.
        void advanceTimers(float deltaSeconds);

        struct PendingTimer {
            TimerId id = invalidTimer;
            float remaining = 0.f;
            std::function<void()> todo;
        };

        Entity owner{};
        World* scene = nullptr;

        std::vector<PendingTimer> timers;
        std::vector<SubscriptionId> subscriptions;
        TimerId nextTimer = 1;
    };

}  // namespace ege
