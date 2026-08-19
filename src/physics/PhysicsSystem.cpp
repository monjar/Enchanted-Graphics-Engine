#include "physics/PhysicsSystem.hpp"

#include "core/Log.hpp"
#include "physics/PhysicsComponents.hpp"
#include "scene/Components.hpp"
#include "scene/Hierarchy.hpp"
#include "scene/TransformInterpolation.hpp"

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <cmath>

namespace ege {

    namespace {

        // EntityId travels through the physics world as the opaque user
        // datum. The raw handle - index and generation together - so a
        // recycled slot resolves as dead rather than as its new occupant.
        std::uint64_t toUserData(EntityId entity) {
            return entity.raw();
        }

        EntityId fromUserData(std::uint64_t data) {
            const auto raw = static_cast<EntityId::Storage>(data);
            return EntityId{raw & EntityId::indexMask, raw >> EntityId::indexBits};
        }

        // The entity's pose and scale in world space. Physics works in world
        // space only; parenting is divided back out when poses are written
        // back.
        struct WorldPose {
            BodyPose pose{};
            glm::vec3 scale{1.f};
        };

        WorldPose worldPoseOf(World& world, EntityId entity) {
            const Transform decomposed =
                Transform::fromMatrix(hierarchy::worldMatrix(world, entity));
            WorldPose result{};
            result.pose.position = decomposed.translation;
            // The same Y-then-X-then-Z composition Transform::mat4 applies.
            result.pose.rotation = glm::angleAxis(decomposed.rotation.y, glm::vec3{0.f, 1.f, 0.f}) *
                                   glm::angleAxis(decomposed.rotation.x, glm::vec3{1.f, 0.f, 0.f}) *
                                   glm::angleAxis(decomposed.rotation.z, glm::vec3{0.f, 0.f, 1.f});
            result.scale = glm::abs(decomposed.scale);
            return result;
        }

        // The collider an entity presents, scaled into world units, or
        // nothing. One collider per entity for now; compound shapes are a
        // backend feature waiting for a caller.
        std::optional<BodyShape> shapeOf(World& world, EntityId entity, glm::vec3 scale) {
            if (const BoxCollider* box = world.find<BoxCollider>(entity)) {
                return BodyShape::box(box->halfExtents * scale, box->offset * scale);
            }
            if (const SphereCollider* sphere = world.find<SphereCollider>(entity)) {
                // A sphere has one radius however the entity is scaled; the
                // largest axis keeps the collider covering the mesh.
                const float radius = std::max({scale.x, scale.y, scale.z});
                return BodyShape::sphere(sphere->radius * radius, sphere->offset * scale);
            }
            if (const CapsuleCollider* capsule = world.find<CapsuleCollider>(entity)) {
                const float radius = std::max(scale.x, scale.z);
                return BodyShape::capsule(
                    capsule->radius * radius,
                    capsule->halfHeight * scale.y,
                    capsule->offset * scale);
            }
            return std::nullopt;
        }

        BodyMotion motionOf(World& world, EntityId entity) {
            if (const RigidBody* rigidBody = world.find<RigidBody>(entity)) {
                return rigidBody->kinematic ? BodyMotion::kinematic : BodyMotion::dynamic;
            }
            // A collider with no RigidBody is scenery: the simulation may
            // land on it but never move it.
            return BodyMotion::stationary;
        }

    }  // namespace

    void PhysicsSystem::start(World& world, const PhysicsWorld::Settings& settings) {
        if (running()) {
            stop(world);
        }
        backend = PhysicsWorld::create(settings);
        world.setPhysics(backend.get());
        reconcile(world);
        EGE_INFO("Physics started: {} bodies", backend->bodyCount());
    }

    void PhysicsSystem::stop(World& world) {
        if (!running()) {
            return;
        }
        // The handles die with the world they point into. The components
        // stay - they are the description play built the bodies from.
        world.each<RigidBody>(
            [](Entity, RigidBody& rigidBody) { rigidBody.body = invalidPhysicsBody; });
        // Nothing is stepping these any more, and Stop is about to put their
        // transforms back where they were before Play. Leaving a pose to
        // interpolate away from would draw one frame sliding from where the
        // simulation had got to towards where the scene says they belong.
        for (const auto& [entity, record] : bodies) {
            world.detach<PreviousTransform>(entity);
        }
        bodies.clear();
        backend.reset();
        world.setPhysics(nullptr);
    }

    std::vector<EntityContact> PhysicsSystem::fixedTick(World& world, float deltaSeconds) {
        if (!running()) {
            return {};
        }

        reconcile(world);

        // Kinematic bodies are handed their Transform as a target before the
        // step, with the velocity the move implies, so whatever they sweep
        // through is pushed rather than skipped over.
        for (const auto& [entity, record] : bodies) {
            if (record.motion == BodyMotion::kinematic && world.alive(entity)) {
                backend->moveKinematic(record.id, worldPoseOf(world, entity).pose, deltaSeconds);
            }
        }

        backend->step(deltaSeconds);

        // Simulation results write back to the transforms the renderer and
        // gameplay read. World space divided back through the parent, since
        // a Transform's fields are local; the entity keeps its own scale,
        // which a rigid body cannot change.
        for (const auto& [entity, record] : bodies) {
            if (record.motion != BodyMotion::dynamic || !world.alive(entity)) {
                continue;
            }
            Transform* transform = world.find<Transform>(entity);
            if (transform == nullptr) {
                continue;
            }

            const BodyPose pose = backend->pose(record.id);
            glm::mat4 bodyWorld = glm::mat4_cast(pose.rotation);
            bodyWorld[3] = glm::vec4{pose.position, 1.f};

            glm::mat4 local = bodyWorld;
            const EntityId parent = hierarchy::parentOf(world, entity);
            if (!parent.isNull()) {
                local = glm::inverse(hierarchy::worldMatrix(world, parent)) * bodyWorld;
            }

            const Transform decomposed = Transform::fromMatrix(local);
            transform->translation = decomposed.translation;
            transform->rotation = decomposed.rotation;
            hierarchy::markDirty(world, entity);
        }

        // Contacts, with both opaque sides resolved back to entities. An
        // entity despawned in the same tick its touch began drops the event:
        // gameplay should not hear from the dead.
        std::vector<EntityContact> contacts;
        for (const ContactEvent& event : backend->drainContacts()) {
            const EntityId a = fromUserData(event.userDataA);
            const EntityId b = fromUserData(event.userDataB);
            if (!world.alive(a) || !world.alive(b)) {
                continue;
            }
            EntityContact contact{};
            contact.a = world.lookup(a);
            contact.b = world.lookup(b);
            contact.point = event.point;
            contact.normal = event.normal;
            contacts.push_back(contact);
        }
        return contacts;
    }

    void PhysicsSystem::reconcile(World& world) {
        // Bodies whose entity died, lost its collider, or changed its mind
        // about how it moves. Collected first: removing while iterating the
        // map being iterated is the usual way to corrupt it.
        std::vector<EntityId> stale;
        for (const auto& [entity, record] : bodies) {
            const bool gone =
                !world.alive(entity) || !shapeOf(world, entity, glm::vec3{1.f}).has_value();
            if (gone || motionOf(world, entity) != record.motion) {
                stale.push_back(entity);
            }
        }
        for (const EntityId entity : stale) {
            backend->removeBody(bodies[entity].id);
            bodies.erase(entity);
            if (RigidBody* rigidBody = world.find<RigidBody>(entity)) {
                rigidBody->body = invalidPhysicsBody;
            }
        }

        // Entities presenting a collider that have no body yet. Bodies are
        // created from the entity's current world pose, so an entity spawned
        // mid-play starts simulating exactly where gameplay put it.
        auto createFor = [&](Entity entity) {
            if (bodies.find(entity.id()) == bodies.end() &&
                world.find<Transform>(entity.id()) != nullptr) {
                createBody(world, entity.id());
            }
        };
        world.each<BoxCollider>([&](Entity entity, BoxCollider&) { createFor(entity); });
        world.each<SphereCollider>([&](Entity entity, SphereCollider&) { createFor(entity); });
        world.each<CapsuleCollider>([&](Entity entity, CapsuleCollider&) { createFor(entity); });
    }

    void PhysicsSystem::createBody(World& world, EntityId entity) {
        const WorldPose worldPose = worldPoseOf(world, entity);
        const std::optional<BodyShape> shape = shapeOf(world, entity, worldPose.scale);
        if (!shape.has_value()) {
            return;
        }

        BodySettings settings{};
        settings.shape = *shape;
        settings.motion = motionOf(world, entity);
        settings.position = worldPose.pose.position;
        settings.rotation = worldPose.pose.rotation;
        settings.userData = toUserData(entity);

        RigidBody* rigidBody = world.find<RigidBody>(entity);
        if (rigidBody != nullptr) {
            settings.mass = rigidBody->mass;
            settings.friction = rigidBody->friction;
            settings.restitution = rigidBody->restitution;
            settings.linearDamping = rigidBody->linearDamping;
            settings.angularDamping = rigidBody->angularDamping;
            settings.gravityFactor = rigidBody->gravityFactor;
            settings.sensor = rigidBody->sensor;
        }

        const PhysicsBodyId body = backend->addBody(settings);
        if (body == invalidPhysicsBody) {
            return;
        }
        bodies[entity] = BodyRecord{body, settings.motion};
        // From here the fixed step decides where this entity is, so the
        // renderer should draw it between steps rather than on them. The
        // component is the opt-in; anything else moved on the fixed clock
        // asks for one the same way.
        beginInterpolating(world, entity);
        if (rigidBody != nullptr) {
            rigidBody->body = body;
        }
    }

}  // namespace ege
