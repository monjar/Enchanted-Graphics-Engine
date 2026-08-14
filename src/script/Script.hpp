#pragma once

#include "reflect/Reflect.hpp"
#include "script/Behavior.hpp"

#include <memory>
#include <string>
#include <vector>

namespace ege {

    // The behaviours attached to an entity.
    //
    // A list inside one component rather than one component per behaviour,
    // because the ECS keys components by type and a project wants two
    // behaviours on the same entity constantly - a controller and a spinner,
    // say. The cost is that the inspector draws this component specially
    // instead of getting it from reflection for free, which is the same trade
    // asset references make and for the same reason: what is stored and what is
    // held at runtime are different things.
    struct Script {
        struct Slot {
            // What a scene file records. The instance is built from it.
            std::string behavior;
            // Null until play begins, or until the editor makes one to show
            // its fields. Shared rather than unique so the component stays
            // copyable, which the component pool's move-and-swap wants.
            std::shared_ptr<Behavior> instance;
            // Set once onSpawn has run, so a behaviour attached mid-play still
            // gets its one call and one already playing does not get a second.
            bool spawned = false;
            // The fields exactly as the scene file held them, kept for the one
            // case where they cannot be reconstructed: a scene naming a
            // behaviour this build does not have. Opening and saving such a
            // scene must not be how its data gets deleted.
            std::string savedFields;
        };

        std::vector<Slot> behaviors;
    };

}  // namespace ege

// Reflected for its name alone: the fields are a vector of polymorphic
// instances, which reflection has no way to describe. Serialization and the
// inspector both handle it specially - see ScriptSerialization and the
// editor's script panel.
EGE_REFLECT(ege::Script)
EGE_REFLECT_END()
