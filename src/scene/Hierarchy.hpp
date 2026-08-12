#pragma once

#include "scene/World.hpp"

#include <glm/glm.hpp>

#include <vector>

namespace ege {

    // Parent-child operations and world-transform resolution.
    //
    // Free functions over the World rather than methods on it, so the ECS core
    // stays unaware of hierarchy. Nothing in World needs to know these exist,
    // and a project that does not want a scene graph pays nothing for it.
    namespace hierarchy {

        // Reparents `child` under `parent`. Passing a null parent detaches it
        // to the root.
        //
        // Refuses to create a cycle - parenting an entity under its own
        // descendant - and returns false in that case rather than building a
        // graph that will hang the next traversal. The editor's drag-and-drop
        // will hit this constantly.
        bool setParent(World& world, EntityId child, EntityId parent);

        EntityId parentOf(World& world, EntityId entity);

        // Direct children, in insertion order.
        std::vector<EntityId> childrenOf(World& world, EntityId entity);

        // Every descendant, depth first.
        std::vector<EntityId> descendantsOf(World& world, EntityId entity);

        bool isDescendantOf(World& world, EntityId entity, EntityId possibleAncestor);

        // Detaches from any parent and unlinks all children, promoting them to
        // the root. Call before despawning, or the siblings' links dangle.
        void unlink(World& world, EntityId entity);

        // Despawns an entity together with its whole subtree.
        void despawnSubtree(World& world, EntityId entity);

        // Marks an entity and everything under it as needing its world matrix
        // recomputed. Call after writing a Transform.
        void markDirty(World& world, EntityId entity);

        // Composed transform, parents applied outermost first. Recomputes only
        // the dirty part of the chain.
        glm::mat4 worldMatrix(World& world, EntityId entity);

        // Resolves every entity's world matrix. Called once per frame before
        // rendering.
        void resolveTransforms(World& world);

    }  // namespace hierarchy

}  // namespace ege
