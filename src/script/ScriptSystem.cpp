#include "script/ScriptSystem.hpp"

#include "core/Log.hpp"
#include "script/BehaviorRegistry.hpp"
#include "script/Script.hpp"

#include <vector>

namespace ege {

    namespace {

        // Behaviour callbacks can spawn and despawn entities, which
        // invalidates the pool iteration they were called from. Collecting the
        // instances first and calling them after is what makes that legal -
        // and gameplay code that spawns things is not an edge case, it is the
        // normal case.
        struct Invocation {
            Entity entity;
            std::shared_ptr<Behavior> behavior;
        };

        std::vector<Invocation> gather(World& world) {
            std::vector<Invocation> out;
            world.each<Script>([&](Entity entity, Script& script) {
                for (Script::Slot& slot : script.behaviors) {
                    if (slot.instance != nullptr && slot.spawned) {
                        out.push_back(Invocation{entity, slot.instance});
                    }
                }
            });
            return out;
        }

        // One entity's spawned behaviours, collected for the same reason
        // gather() collects everything: an onContact may spawn or despawn,
        // and must not do so inside the walk that found it.
        std::vector<std::shared_ptr<Behavior>> behaviorsOf(World& world, EntityId entity) {
            std::vector<std::shared_ptr<Behavior>> out;
            if (!world.alive(entity)) {
                return out;
            }
            if (Script* script = world.find<Script>(entity)) {
                for (Script::Slot& slot : script->behaviors) {
                    if (slot.instance != nullptr && slot.spawned) {
                        out.push_back(slot.instance);
                    }
                }
            }
            return out;
        }

    }  // namespace

    void ScriptSystem::spawnPending(World& world) {
        std::vector<Invocation> starting;

        world.each<Script>([&](Entity entity, Script& script) {
            for (Script::Slot& slot : script.behaviors) {
                if (slot.spawned) {
                    continue;
                }
                if (slot.instance == nullptr) {
                    const BehaviorRegistry::Entry* entry =
                        BehaviorRegistry::instance().find(slot.behavior);
                    if (entry == nullptr) {
                        // Named once, not once per frame: a scene referring to
                        // a behaviour this build does not have is worth
                        // hearing about, and worth hearing about quietly.
                        EGE_WARN("no behaviour named '{}'; skipping it", slot.behavior);
                        slot.spawned = true;
                        continue;
                    }
                    slot.instance = entry->create();
                }
                slot.instance->owner = entity;
                slot.instance->scene = &world;
                slot.spawned = true;
                starting.push_back(Invocation{entity, slot.instance});
            }
        });

        for (const Invocation& invocation : starting) {
            invocation.behavior->onSpawn();
        }
    }

    void ScriptSystem::tick(World& world, float deltaSeconds) {
        for (const Invocation& invocation : gather(world)) {
            if (world.alive(invocation.entity.id())) {
                invocation.behavior->onTick(deltaSeconds);
            }
        }
    }

    void ScriptSystem::fixedTick(World& world, float deltaSeconds) {
        for (const Invocation& invocation : gather(world)) {
            if (world.alive(invocation.entity.id())) {
                invocation.behavior->onFixedTick(deltaSeconds);
            }
        }
    }

    void ScriptSystem::deliverContacts(World& world, const std::vector<EntityContact>& contacts) {
        for (const EntityContact& event : contacts) {
            // Re-checked per event, not just per batch: an earlier onContact
            // may have despawned either side, and the dead take no calls.
            for (const std::shared_ptr<Behavior>& behavior : behaviorsOf(world, event.a.id())) {
                if (world.alive(event.a.id()) && world.alive(event.b.id())) {
                    Contact contact{};
                    contact.other = event.b;
                    contact.point = event.point;
                    contact.normal = event.normal;
                    behavior->onContact(contact);
                }
            }
            for (const std::shared_ptr<Behavior>& behavior : behaviorsOf(world, event.b.id())) {
                if (world.alive(event.a.id()) && world.alive(event.b.id())) {
                    Contact contact{};
                    contact.other = event.a;
                    contact.point = event.point;
                    // The physics system's normal points from a to b; from
                    // b's side the other entity lies the opposite way.
                    contact.normal = -event.normal;
                    behavior->onContact(contact);
                }
            }
        }
    }

    void ScriptSystem::despawnAll(World& world) {
        for (const Invocation& invocation : gather(world)) {
            invocation.behavior->onDespawn();
        }

        world.each<Script>([](Entity, Script& script) {
            for (Script::Slot& slot : script.behaviors) {
                slot.instance.reset();
                slot.spawned = false;
            }
        });
    }

}  // namespace ege
