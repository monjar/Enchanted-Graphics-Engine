#include "scene/Systems.hpp"

#include "scene/Components.hpp"
#include "scene/Hierarchy.hpp"

namespace ege::systems {

    void spin(World& world, float deltaSeconds) {
        world.each<Transform, Spin>([&](Entity entity, Transform& transform, Spin& motion) {
            transform.rotation += motion.anglesPerSecond * deltaSeconds;
            // The world matrix is cached behind a dirty flag, so a system that
            // writes a Transform has to say so - exactly as the inspector does.
            hierarchy::markDirty(world, entity.id());
        });
    }

}  // namespace ege::systems
