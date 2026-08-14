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
