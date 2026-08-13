#include "scene/Hierarchy.hpp"

#include "core/Log.hpp"
#include "scene/Components.hpp"

namespace ege {

    namespace hierarchy {

        namespace {

            // Every hierarchy operation needs the component; entities without
            // one are simply roots with no children.
            Hierarchy& ensure(World& world, EntityId entity) {
                if (Hierarchy* existing = world.find<Hierarchy>(entity)) {
                    return *existing;
                }
                return world.attach<Hierarchy>(entity);
            }

            void unlinkFromParent(World& world, EntityId entity) {
                Hierarchy* node = world.find<Hierarchy>(entity);
                if (node == nullptr || node->parent.isNull()) {
                    return;
                }

                Hierarchy* parentNode = world.find<Hierarchy>(node->parent);

                // Repair the sibling chain around the departing entity. Keeping
                // a previous pointer is what makes this constant time rather
                // than a scan from the parent's first child.
                if (!node->previousSibling.isNull()) {
                    if (Hierarchy* previous = world.find<Hierarchy>(node->previousSibling)) {
                        previous->nextSibling = node->nextSibling;
                    }
                } else if (parentNode != nullptr) {
                    parentNode->firstChild = node->nextSibling;
                }

                if (!node->nextSibling.isNull()) {
                    if (Hierarchy* next = world.find<Hierarchy>(node->nextSibling)) {
                        next->previousSibling = node->previousSibling;
                    }
                }

                node->parent = EntityId{};
                node->nextSibling = EntityId{};
                node->previousSibling = EntityId{};
            }

        }  // namespace

        bool isDescendantOf(World& world, EntityId entity, EntityId possibleAncestor) {
            if (possibleAncestor.isNull()) {
                return false;
            }

            EntityId current = entity;
            // Bounded by the entity count so a corrupted chain cannot spin
            // forever, which matters because this is the cycle guard itself.
            std::size_t guard = 0;
            const std::size_t limit = world.entityCount() + 1;

            while (!current.isNull() && guard++ <= limit) {
                if (current == possibleAncestor) {
                    return true;
                }
                Hierarchy* node = world.find<Hierarchy>(current);
                current = node == nullptr ? EntityId{} : node->parent;
            }
            return false;
        }

        bool setParent(World& world, EntityId child, EntityId parent) {
            if (!world.alive(child)) {
                return false;
            }
            if (child == parent) {
                EGE_WARN("refusing to parent an entity to itself");
                return false;
            }
            if (!parent.isNull() && !world.alive(parent)) {
                return false;
            }
            // Parenting under a descendant would make the chain a loop, and
            // every traversal after that would hang.
            if (!parent.isNull() && isDescendantOf(world, parent, child)) {
                EGE_WARN("refusing to parent an entity under its own descendant");
                return false;
            }

            unlinkFromParent(world, child);

            Hierarchy& childNode = ensure(world, child);
            childNode.parent = parent;
            childNode.dirty = true;

            if (!parent.isNull()) {
                Hierarchy& parentNode = ensure(world, parent);

                // Append rather than prepend, so childrenOf reports insertion
                // order - which is what a hierarchy panel should show.
                if (parentNode.firstChild.isNull()) {
                    parentNode.firstChild = child;
                } else {
                    EntityId sibling = parentNode.firstChild;
                    while (true) {
                        Hierarchy* siblingNode = world.find<Hierarchy>(sibling);
                        if (siblingNode == nullptr || siblingNode->nextSibling.isNull()) {
                            if (siblingNode != nullptr) {
                                siblingNode->nextSibling = child;
                            }
                            // Re-fetched because ensure() above may have grown
                            // the pool and moved the component.
                            if (Hierarchy* refreshedChild = world.find<Hierarchy>(child)) {
                                refreshedChild->previousSibling = sibling;
                            }
                            break;
                        }
                        sibling = siblingNode->nextSibling;
                    }
                }
            }

            markDirty(world, child);
            return true;
        }

        EntityId parentOf(World& world, EntityId entity) {
            Hierarchy* node = world.find<Hierarchy>(entity);
            return node == nullptr ? EntityId{} : node->parent;
        }

        std::vector<EntityId> childrenOf(World& world, EntityId entity) {
            std::vector<EntityId> children;
            Hierarchy* node = world.find<Hierarchy>(entity);
            if (node == nullptr) {
                return children;
            }

            EntityId current = node->firstChild;
            while (!current.isNull()) {
                children.push_back(current);
                Hierarchy* childNode = world.find<Hierarchy>(current);
                current = childNode == nullptr ? EntityId{} : childNode->nextSibling;
            }
            return children;
        }

        std::vector<EntityId> descendantsOf(World& world, EntityId entity) {
            std::vector<EntityId> result;
            std::vector<EntityId> stack = childrenOf(world, entity);

            while (!stack.empty()) {
                const EntityId current = stack.back();
                stack.pop_back();
                result.push_back(current);
                for (EntityId child : childrenOf(world, current)) {
                    stack.push_back(child);
                }
            }
            return result;
        }

        void unlink(World& world, EntityId entity) {
            // Children are promoted to the root rather than orphaned, so their
            // parent links never point at a despawned entity.
            for (EntityId child : childrenOf(world, entity)) {
                setParent(world, child, EntityId{});
            }
            unlinkFromParent(world, entity);
        }

        void despawnSubtree(World& world, EntityId entity) {
            const std::vector<EntityId> doomed = descendantsOf(world, entity);
            // Deepest first, so each unlink sees an intact chain below it.
            for (auto it = doomed.rbegin(); it != doomed.rend(); ++it) {
                unlink(world, *it);
                world.despawn(*it);
            }
            unlink(world, entity);
            world.despawn(entity);
        }

        void markDirty(World& world, EntityId entity) {
            Hierarchy* node = world.find<Hierarchy>(entity);
            if (node == nullptr) {
                return;
            }
            node->dirty = true;
            for (EntityId child : childrenOf(world, entity)) {
                markDirty(world, child);
            }
        }

        glm::mat4 worldMatrix(World& world, EntityId entity) {
            Transform* transform = world.find<Transform>(entity);
            const glm::mat4 local = transform == nullptr ? glm::mat4{1.f} : transform->mat4();

            Hierarchy* node = world.find<Hierarchy>(entity);
            if (node == nullptr) {
                return local;
            }
            if (!node->dirty) {
                return node->worldMatrix;
            }

            // Parent first, so its cache is warm before this one is written.
            const glm::mat4 parentMatrix =
                node->parent.isNull() ? glm::mat4{1.f} : worldMatrix(world, node->parent);

            // find<Hierarchy> again: the recursive call may have attached a
            // Hierarchy further up, and a component pool can reallocate when it
            // grows, which would leave `node` dangling.
            Hierarchy* refreshed = world.find<Hierarchy>(entity);
            if (refreshed == nullptr) {
                // Cannot happen - the component existed a moment ago and
                // nothing detaches it here - but returning the composed matrix
                // uncached is the harmless answer if it ever does.
                return parentMatrix * local;
            }
            refreshed->worldMatrix = parentMatrix * local;
            refreshed->dirty = false;
            return refreshed->worldMatrix;
        }

        void resolveTransforms(World& world) {
            for (Entity entity : world.all()) {
                if (world.has<Hierarchy>(entity.id())) {
                    worldMatrix(world, entity.id());
                }
            }
        }

    }  // namespace hierarchy

}  // namespace ege
