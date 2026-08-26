#include "physics/PhysicsSystem.hpp"

#include "core/Log.hpp"
#include "physics/CharacterMotion.hpp"
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
        EGE_INFO(
            "Physics started: {} bodies, {} characters",
            backend->bodyCount(),
            backend->characterCount());
    }

    void PhysicsSystem::stop(World& world) {
        if (!running()) {
            return;
        }
        // The handles die with the world they point into. The components
        // stay - they are the description play built the bodies from.
        world.each<RigidBody>(
            [](Entity, RigidBody& rigidBody) { rigidBody.body = invalidPhysicsBody; });
        world.each<CharacterController>([](Entity, CharacterController& controller) {
            controller.character = invalidPhysicsCharacter;
            // Everything a run accumulated: mid-air, mid-jump, walking at
            // six metres per second. Stop puts the transform back, and a
            // character that came back to its starting mark still falling is
            // the same leak the transform restore exists to prevent.
            controller.velocity = glm::vec3{0.f};
            controller.grounded = false;
            controller.jumped = false;
            controller.planarSpeed = 0.f;
            controller.coyote = 0.f;
            controller.buffered = 0.f;
        });
        // Nothing is stepping these any more, and Stop is about to put their
        // transforms back where they were before Play. Leaving a pose to
        // interpolate away from would draw one frame sliding from where the
        // simulation had got to towards where the scene says they belong.
        for (const auto& [entity, record] : bodies) {
            world.detach<PreviousTransform>(entity);
        }
        for (const auto& [entity, record] : characters) {
            world.detach<PreviousTransform>(entity);
        }
        bodies.clear();
        characters.clear();
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

        // Characters move before the step rather than after it, so that the
        // shove a character gives a crate is integrated by the step that
        // follows rather than sitting on the body for a frame.
        moveCharacters(world, deltaSeconds);

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

        // The same reconciliation for characters, which are their own kind of
        // object rather than a body with a component on it.
        std::vector<EntityId> lostCharacters;
        for (const auto& [entity, record] : characters) {
            if (!world.alive(entity) || world.find<CharacterController>(entity) == nullptr) {
                lostCharacters.push_back(entity);
            }
        }
        for (const EntityId entity : lostCharacters) {
            backend->removeCharacter(characters[entity].id);
            characters.erase(entity);
            if (CharacterController* controller = world.find<CharacterController>(entity)) {
                controller->character = invalidPhysicsCharacter;
            }
        }

        world.each<CharacterController>([&](Entity entity, CharacterController&) {
            if (characters.find(entity.id()) == characters.end() &&
                world.find<Transform>(entity.id()) != nullptr) {
                createCharacter(world, entity.id());
            }
        });
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

    void PhysicsSystem::createCharacter(World& world, EntityId entity) {
        CharacterController* controller = world.find<CharacterController>(entity);
        if (controller == nullptr) {
            return;
        }
        const WorldPose worldPose = worldPoseOf(world, entity);

        CharacterSettings settings{};
        // Scaled like every other collider: a capsule fitted to a mesh stays
        // fitted when the entity is scaled. The radius takes the larger of
        // the two horizontal axes, because a capsule has one.
        settings.radius = controller->radius * std::max(worldPose.scale.x, worldPose.scale.z);
        settings.halfHeight = controller->halfHeight * worldPose.scale.y;
        settings.position = worldPose.pose.position;
        // Up is the world's, not the entity's: which way a character falls is
        // decided by gravity, and a character standing on its head is a
        // rotated mesh rather than a rotated simulation.
        settings.up = upFromGravity(backend->gravity());
        settings.maxSlopeAngle = controller->maxSlopeAngle;
        settings.stepHeight = controller->stepHeight * worldPose.scale.y;
        settings.stickToFloor = controller->stickToFloor * worldPose.scale.y;
        settings.mass = controller->mass;
        settings.pushForce = controller->pushForce;
        settings.userData = toUserData(entity);

        const PhysicsCharacterId id = backend->addCharacter(settings);
        if (id == invalidPhysicsCharacter) {
            return;
        }
        characters[entity] = CharacterRecord{id, worldPose.pose.position};
        controller->character = id;
        // Moved by the fixed step, so drawn between them - the same opt-in a
        // simulated body makes.
        beginInterpolating(world, entity);
    }

    void PhysicsSystem::moveCharacters(World& world, float deltaSeconds) {
        const glm::vec3 gravity = backend->gravity();
        const glm::vec3 up = upFromGravity(gravity);
        const float pull = glm::length(gravity);

        for (auto& [entity, record] : characters) {
            if (!world.alive(entity)) {
                continue;
            }
            CharacterController* controller = world.find<CharacterController>(entity);
            Transform* transform = world.find<Transform>(entity);
            if (controller == nullptr || transform == nullptr) {
                continue;
            }

            // A transform written since the last step is a teleport - a
            // respawn, a gizmo drag, a script putting the character
            // somewhere. Compared against where this system last left it
            // rather than against the capsule, so that the ordinary case of
            // nobody touching it is not read as a teleport back to where the
            // simulation already was.
            const glm::vec3 authored = worldPoseOf(world, entity).pose.position;
            if (glm::distance(authored, record.position) > 1e-4f) {
                backend->setCharacterPosition(record.id, authored);
                controller->velocity = glm::vec3{0.f};
            }

            const CharacterGround ground = backend->characterGround(record.id);

            CharacterFrame frame{};
            frame.up = up;
            frame.gravity = pull;
            frame.grounded = ground.state == CharacterGroundState::grounded;
            frame.groundNormal =
                ground.state == CharacterGroundState::airborne ? up : ground.normal;
            frame.groundVelocity = ground.velocity;

            // What the driver asked for becomes a velocity here, and only
            // here: the arithmetic is the engine's, tested without a device,
            // and the backend is only asked to find out how far that velocity
            // gets.
            advanceCharacter(*controller, frame, deltaSeconds);

            backend->setCharacterVelocity(record.id, controller->velocity);
            backend->updateCharacter(record.id, deltaSeconds);

            // Read back what it managed rather than what it wanted. Walking
            // into a wall has to arrive on the component as a stop, or the
            // next step accelerates from a speed the character never had.
            controller->velocity = backend->characterVelocity(record.id);
            record.position = backend->characterPosition(record.id);

            const CharacterGround landed = backend->characterGround(record.id);
            controller->grounded = landed.state == CharacterGroundState::grounded;
            controller->groundNormal =
                landed.state == CharacterGroundState::airborne ? up : landed.normal;

            // World space divided back through the parent, exactly as a
            // body's pose is. The character's own facing is a yaw about Y,
            // which is where a Transform keeps one.
            glm::mat4 characterWorld{1.f};
            characterWorld[3] = glm::vec4{record.position, 1.f};
            glm::mat4 local = characterWorld;
            const EntityId parent = hierarchy::parentOf(world, entity);
            if (!parent.isNull()) {
                local = glm::inverse(hierarchy::worldMatrix(world, parent)) * characterWorld;
            }
            transform->translation = glm::vec3{local[3]};
            if (controller->faceMotion) {
                transform->rotation.y = controller->facing;
            }
            hierarchy::markDirty(world, entity);
        }
    }

}  // namespace ege
