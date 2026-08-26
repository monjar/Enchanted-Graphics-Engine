#include "core/Log.hpp"
#include "physics/PhysicsWorld.hpp"

// Jolt.h must come first and stay first: every other Jolt header assumes its
// configuration macros are already in place. The comment below keeps the
// formatter from sorting the rest of the pack above it.
#include <Jolt/Jolt.h>

// The rest of Jolt, only after Jolt.h.
#include <Jolt/Core/Factory.h>
#include <Jolt/Core/JobSystemSingleThreaded.h>
#include <Jolt/Core/TempAllocator.h>
#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Body/BodyInterface.h>
#include <Jolt/Physics/Character/CharacterVirtual.h>
#include <Jolt/Physics/Collision/CastResult.h>
#include <Jolt/Physics/Collision/RayCast.h>
#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Physics/Collision/Shape/CapsuleShape.h>
#include <Jolt/Physics/Collision/Shape/RotatedTranslatedShape.h>
#include <Jolt/Physics/Collision/Shape/SphereShape.h>
#include <Jolt/Physics/PhysicsSettings.h>
#include <Jolt/Physics/PhysicsSystem.h>
#include <Jolt/RegisterTypes.h>

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <map>
#include <mutex>
#include <unordered_map>
#include <utility>

namespace ege {

    namespace {

        // ---- Conversions ---------------------------------------------------

        JPH::Vec3 toJolt(glm::vec3 v) {
            return JPH::Vec3{v.x, v.y, v.z};
        }

        JPH::Quat toJolt(glm::quat q) {
            return JPH::Quat{q.x, q.y, q.z, q.w};
        }

        glm::vec3 fromJolt(JPH::Vec3 v) {
            return glm::vec3{v.GetX(), v.GetY(), v.GetZ()};
        }

        glm::quat fromJolt(JPH::Quat q) {
            return glm::quat{q.GetW(), q.GetX(), q.GetY(), q.GetZ()};
        }

        // ---- Layers --------------------------------------------------------

        // Two kinds of layer are at work here and they answer different
        // questions.
        //
        // The *broad phase* layer says whether something moves. Its only job
        // is to let the broad phase skip static-versus-static pairs, which
        // can never collide however the matrix is set - a pure optimisation
        // nobody outside this file names.
        //
        // The *collision* layer is the one a designer names, and the matrix
        // between those is gameplay's to decide. Both live in one Jolt object
        // layer: the collision layer shifted up by one, with "this thing
        // moves" in the bottom bit. Sixteen collision layers and one bit make
        // thirty-two object layers, which is well inside Jolt's sixteen.
        namespace objectLayers {
            constexpr JPH::ObjectLayer movingBit = 1;

            JPH::ObjectLayer encode(CollisionLayer layer, bool moving) {
                return static_cast<JPH::ObjectLayer>(
                    (static_cast<JPH::ObjectLayer>(layer) << 1) |
                    (moving ? movingBit : JPH::ObjectLayer{0}));
            }

            CollisionLayer collisionLayerOf(JPH::ObjectLayer layer) {
                return static_cast<CollisionLayer>(layer >> 1);
            }

            bool movesOf(JPH::ObjectLayer layer) {
                return (layer & movingBit) != 0;
            }
        }  // namespace objectLayers

        namespace broadPhaseLayers {
            constexpr JPH::BroadPhaseLayer nonMoving{0};
            constexpr JPH::BroadPhaseLayer moving{1};
            constexpr JPH::uint count = 2;
        }  // namespace broadPhaseLayers

        class BroadPhaseLayerMap final : public JPH::BroadPhaseLayerInterface {
        public:
            JPH::uint GetNumBroadPhaseLayers() const override { return broadPhaseLayers::count; }

            JPH::BroadPhaseLayer GetBroadPhaseLayer(JPH::ObjectLayer layer) const override {
                return objectLayers::movesOf(layer) ? broadPhaseLayers::moving
                                                    : broadPhaseLayers::nonMoving;
            }

#if defined(JPH_EXTERNAL_PROFILE) || defined(JPH_PROFILE_ENABLED)
            const char* GetBroadPhaseLayerName(JPH::BroadPhaseLayer layer) const override {
                return layer == broadPhaseLayers::nonMoving ? "nonMoving" : "moving";
            }
#endif
        };

        class ObjectVsBroadPhaseFilter final : public JPH::ObjectVsBroadPhaseLayerFilter {
        public:
            bool ShouldCollide(
                JPH::ObjectLayer layer, JPH::BroadPhaseLayer broadPhase) const override {
                if (!objectLayers::movesOf(layer)) {
                    return broadPhase == broadPhaseLayers::moving;
                }
                return true;
            }
        };

        // The matrix, asked one pair at a time. Holds a pointer rather than a
        // copy because Jolt calls this from its own threads throughout a
        // step: the layers are fixed for the world's lifetime, so a read
        // needs no lock, and a copy would be a second thing to keep in step.
        class ObjectLayerFilter final : public JPH::ObjectLayerPairFilter {
        public:
            explicit ObjectLayerFilter(const CollisionLayers& layersRef) : layers{&layersRef} {}

            bool ShouldCollide(JPH::ObjectLayer a, JPH::ObjectLayer b) const override {
                // Neither moves: the broad phase would have skipped this pair
                // anyway, and asking the matrix about it would let a designer
                // turn on a collision that can never happen.
                if (!objectLayers::movesOf(a) && !objectLayers::movesOf(b)) {
                    return false;
                }
                return layers->collides(
                    objectLayers::collisionLayerOf(a), objectLayers::collisionLayerOf(b));
            }

        private:
            const CollisionLayers* layers;
        };

        // ---- Process-wide Jolt state ---------------------------------------

        // A real function rather than a lambda: a variadic lambda cannot decay
        // to the function pointer JPH::Trace is.
        //
        // Declared printf-like so the compiler knows `fmt` is a format string
        // checked at whatever called this. Without that, handing it on to
        // vsnprintf is a non-literal format and -Wformat=2 rejects it - which
        // clang does and GCC does not, so it surfaced the day a Mac joined CI.
#if defined(__GNUC__) || defined(__clang__)
        __attribute__((format(printf, 1, 2)))
#endif
        void
        traceToLog(const char* fmt, ...) {
            std::va_list args;
            va_start(args, fmt);
            char buffer[1024];
            std::vsnprintf(buffer, sizeof(buffer), fmt, args);
            va_end(args);
            EGE_INFO("Jolt: {}", buffer);
        }

        // Allocator, type factory and RTTI registration are process-global in
        // Jolt and must exist before the first PhysicsSystem. Registered once
        // and left in place for the process lifetime, the same arrangement the
        // engine's own registries use; tests create and destroy many worlds
        // and must not re-register per world.
        void ensureJoltRegistered() {
            static std::once_flag once;
            std::call_once(once, [] {
                JPH::RegisterDefaultAllocator();
                JPH::Trace = traceToLog;
#ifdef JPH_ENABLE_ASSERTS
                JPH::AssertFailed = [](const char* expression,
                                       const char* message,
                                       const char* file,
                                       JPH::uint line) {
                    EGE_ERROR(
                        "Jolt assert: {} ({}) at {}:{}",
                        expression,
                        message != nullptr ? message : "",
                        file,
                        line);
                    return true;  // break into the debugger if one is attached
                };
#endif
                JPH::Factory::sInstance = new JPH::Factory();
                JPH::RegisterTypes();
            });
        }

        // Buffers contacts raised during a step: gameplay state must not be
        // touched from inside the solver, so events are queued and handed
        // over after the step. The lock is held because Jolt calls listeners
        // from whatever threads its job system runs - today that is the
        // stepping thread only, but this class must not silently become
        // wrong the day the backend grows a worker pool.
        //
        // Jolt reports a contact per pair of *sub-shapes*, and gameplay means
        // a pair of *bodies*: a crate landing flat on the floor arrives here
        // four times and is one thing that happened. So each pair is counted,
        // and only the first arrival and the last departure are events. That
        // counting is also the only way to know a touch ended, since Jolt's
        // removal callback hands over sub-shape ids and nothing else - not
        // even the bodies, which may already be gone.
        class ContactCollector final : public JPH::ContactListener {
        public:
            void OnContactAdded(
                const JPH::Body& bodyA,
                const JPH::Body& bodyB,
                const JPH::ContactManifold& manifold,
                JPH::ContactSettings&) override {
                const std::lock_guard<std::mutex> lock{mutex};
                Touch& touch = touching[pairOf(bodyA.GetID(), bodyB.GetID())];
                if (++touch.corners != 1) {
                    // Another corner of the same two objects.
                    return;
                }

                // Whose touch this is, remembered here rather than looked up
                // when it ends: by then a body may have been destroyed - and
                // being destroyed inside a trigger is one of the ways to
                // leave one - so the name has to outlive the body.
                touch.userDataA = bodyA.GetUserData();
                touch.userDataB = bodyB.GetUserData();

                ContactEvent event{};
                event.userDataA = touch.userDataA;
                event.userDataB = touch.userDataB;
                event.point = fromJolt(manifold.GetWorldSpaceContactPointOn1(0));
                event.normal = fromJolt(manifold.mWorldSpaceNormal);
                events.push_back(event);
            }

            void OnContactRemoved(const JPH::SubShapeIDPair& pairIds) override {
                const std::lock_guard<std::mutex> lock{mutex};
                const auto found =
                    touching.find(pairOf(pairIds.GetBody1ID(), pairIds.GetBody2ID()));
                if (found == touching.end()) {
                    return;
                }
                if (--found->second.corners > 0) {
                    return;
                }

                SeparationEvent event{};
                event.userDataA = found->second.userDataA;
                event.userDataB = found->second.userDataB;
                separations.push_back(event);
                touching.erase(found);
            }

            std::vector<ContactEvent> drain() {
                const std::lock_guard<std::mutex> lock{mutex};
                std::vector<ContactEvent> out = std::move(events);
                events.clear();
                return out;
            }

            std::vector<SeparationEvent> drainSeparations() {
                const std::lock_guard<std::mutex> lock{mutex};
                std::vector<SeparationEvent> out = std::move(separations);
                separations.clear();
                return out;
            }

        private:
            // Ordered, so that the same two bodies key the same entry however
            // they arrive.
            using Pair = std::pair<std::uint32_t, std::uint32_t>;

            // How many sub-shape pairs of these two bodies are touching, and
            // who they were when the touch began.
            struct Touch {
                int corners = 0;
                std::uint64_t userDataA = 0;
                std::uint64_t userDataB = 0;
            };

            static Pair pairOf(JPH::BodyID a, JPH::BodyID b) {
                const std::uint32_t first = a.GetIndexAndSequenceNumber();
                const std::uint32_t second = b.GetIndexAndSequenceNumber();
                return first <= second ? Pair{first, second} : Pair{second, first};
            }

            std::mutex mutex;
            std::vector<ContactEvent> events;
            std::vector<SeparationEvent> separations;
            // Bounded by what is actually touching: an entry appears with the
            // first corner and is gone with the last, so nothing accumulates
            // over a long session.
            std::map<Pair, Touch> touching;
        };

        // ---- The backend ---------------------------------------------------

        class JoltPhysicsWorld final : public PhysicsWorld {
        public:
            explicit JoltPhysicsWorld(const Settings& settings)
                // The step runs on the calling thread, no worker pool. The
                // scenes this engine simulates are nowhere near paying for
                // one, Jolt is deterministic regardless of thread count so
                // nothing changes when one arrives, and a single-threaded
                // step is one ThreadSanitizer can actually see through -
                // Jolt's lock-free internals synchronise with bare fences,
                // which TSan cannot model and would report as races.
                : worldGravity{settings.gravity},
                  worldLayers{settings.layers},
                  objectPairs{worldLayers},
                  tempAllocator{tempAllocatorBytes},
                  jobs{JPH::cMaxPhysicsJobs} {
                system.Init(
                    settings.maxBodies,
                    0,
                    settings.maxBodies,
                    settings.maxBodies,
                    broadPhaseMap,
                    objectVsBroadPhase,
                    objectPairs);
                system.SetGravity(toJolt(settings.gravity));
                system.SetContactListener(&contacts);
            }

            ~JoltPhysicsWorld() override {
                // Characters hold a reference to the system they collide
                // against, so they go first.
                characters.clear();

                // Bodies must be removed before the system is destroyed;
                // destroying the interface-issued handles is enough.
                JPH::BodyInterface& bodies = system.GetBodyInterface();
                JPH::BodyIDVector all;
                system.GetBodies(all);
                for (const JPH::BodyID& body : all) {
                    bodies.RemoveBody(body);
                    bodies.DestroyBody(body);
                }
            }

            PhysicsBodyId addBody(const BodySettings& settings) override {
                JPH::ShapeRefC shape = buildShape(settings.shape);
                if (shape == nullptr) {
                    return invalidPhysicsBody;
                }

                const JPH::EMotionType motion =
                    settings.motion == BodyMotion::dynamic     ? JPH::EMotionType::Dynamic
                    : settings.motion == BodyMotion::kinematic ? JPH::EMotionType::Kinematic
                                                               : JPH::EMotionType::Static;
                const JPH::ObjectLayer layer =
                    objectLayers::encode(settings.layer, settings.motion != BodyMotion::stationary);

                JPH::BodyCreationSettings creation{
                    shape,
                    toJolt(settings.position),
                    toJolt(glm::normalize(settings.rotation)),
                    motion,
                    layer};
                creation.mFriction = settings.friction;
                creation.mRestitution = settings.restitution;
                creation.mLinearDamping = settings.linearDamping;
                creation.mAngularDamping = settings.angularDamping;
                creation.mGravityFactor = settings.gravityFactor;
                creation.mIsSensor = settings.sensor;
                creation.mUserData = settings.userData;
                if (settings.motion == BodyMotion::dynamic) {
                    creation.mOverrideMassProperties =
                        JPH::EOverrideMassProperties::CalculateInertia;
                    creation.mMassPropertiesOverride.mMass = std::max(settings.mass, 1e-3f);
                }

                JPH::BodyInterface& bodies = system.GetBodyInterface();
                const JPH::BodyID body = bodies.CreateAndAddBody(
                    creation,
                    settings.motion == BodyMotion::stationary ? JPH::EActivation::DontActivate
                                                              : JPH::EActivation::Activate);
                if (body.IsInvalid()) {
                    EGE_ERROR("physics world is full; body not created");
                    return invalidPhysicsBody;
                }
                return body.GetIndexAndSequenceNumber();
            }

            void removeBody(PhysicsBodyId body) override {
                if (body == invalidPhysicsBody) {
                    return;
                }
                JPH::BodyInterface& bodies = system.GetBodyInterface();
                const JPH::BodyID id{body};
                bodies.RemoveBody(id);
                bodies.DestroyBody(id);
            }

            std::size_t bodyCount() const override { return system.GetNumBodies(); }

            BodyPose pose(PhysicsBodyId body) const override {
                JPH::RVec3 position;
                JPH::Quat rotation;
                system.GetBodyInterfaceNoLock().GetPositionAndRotation(
                    JPH::BodyID{body}, position, rotation);
                return BodyPose{fromJolt(position), fromJolt(rotation)};
            }

            void setPose(PhysicsBodyId body, const BodyPose& target) override {
                system.GetBodyInterface().SetPositionAndRotation(
                    JPH::BodyID{body},
                    toJolt(target.position),
                    toJolt(glm::normalize(target.rotation)),
                    JPH::EActivation::Activate);
            }

            void moveKinematic(
                PhysicsBodyId body, const BodyPose& target, float deltaSeconds) override {
                system.GetBodyInterface().MoveKinematic(
                    JPH::BodyID{body},
                    toJolt(target.position),
                    toJolt(glm::normalize(target.rotation)),
                    deltaSeconds);
            }

            glm::vec3 linearVelocity(PhysicsBodyId body) const override {
                return fromJolt(
                    system.GetBodyInterfaceNoLock().GetLinearVelocity(JPH::BodyID{body}));
            }

            void setLinearVelocity(PhysicsBodyId body, glm::vec3 velocity) override {
                JPH::BodyInterface& bodies = system.GetBodyInterface();
                const JPH::BodyID id{body};
                bodies.SetLinearVelocity(id, toJolt(velocity));
                bodies.ActivateBody(id);
            }

            void addImpulse(PhysicsBodyId body, glm::vec3 impulse) override {
                // AddImpulse activates the body itself.
                system.GetBodyInterface().AddImpulse(JPH::BodyID{body}, toJolt(impulse));
            }

            // ---- Characters ---------------------------------------------

            PhysicsCharacterId addCharacter(const CharacterSettings& settings) override {
                const glm::vec3 up = normalisedUp(settings.up);
                JPH::ShapeRefC shape = buildCharacterShape(settings, up);
                if (shape == nullptr) {
                    return invalidPhysicsCharacter;
                }

                JPH::CharacterVirtualSettings creation{};
                creation.mShape = shape;
                creation.mUp = toJolt(up);
                creation.mMaxSlopeAngle = settings.maxSlopeAngle;
                creation.mMass = settings.mass;
                creation.mMaxStrength = settings.pushForce;
                // Which contacts count as standing on something. Jolt's
                // default accepts any contact at all, which means a hand
                // against a wall reads as ground. The capsule's origin is at
                // its centre, so the floor is everything below the bottom
                // cap's own centre - one half-height down.
                creation.mSupportingVolume = JPH::Plane{toJolt(up), settings.halfHeight};
                // Ghost contacts against the internal edges of the box soup a
                // level is made of are what make a character stutter across a
                // flat floor built from two triangles.
                creation.mEnhancedInternalEdgeRemoval = true;
                if (settings.proxyBody) {
                    // A kinematic body of the same shape, kept on the
                    // character by Jolt, so that everything else in the world
                    // can see it. Jolt creates it awake and keeps it that
                    // way, which is what makes a sensor notice a character
                    // standing still inside it.
                    creation.mInnerBodyShape = shape;
                    creation.mInnerBodyLayer = objectLayers::encode(settings.layer, true);
                }

                const PhysicsCharacterId id = nextCharacter++;
                CharacterRecord record{};
                record.up = up;
                record.stepHeight = settings.stepHeight;
                record.stickToFloor = settings.stickToFloor;
                record.layer = objectLayers::encode(settings.layer, true);
                record.character = new JPH::CharacterVirtual{
                    &creation,
                    toJolt(settings.position),
                    JPH::Quat::sIdentity(),
                    settings.userData,
                    &system};
                characters.emplace(id, std::move(record));
                return id;
            }

            void removeCharacter(PhysicsCharacterId character) override {
                characters.erase(character);
            }

            std::size_t characterCount() const override { return characters.size(); }

            glm::vec3 characterPosition(PhysicsCharacterId character) const override {
                const CharacterRecord* record = findCharacter(character);
                return record == nullptr ? glm::vec3{0.f}
                                         : fromJolt(record->character->GetPosition());
            }

            void setCharacterPosition(PhysicsCharacterId character, glm::vec3 position) override {
                CharacterRecord* record = findCharacter(character);
                if (record == nullptr) {
                    return;
                }
                record->character->SetPosition(toJolt(position));
                // A teleported character's idea of what is underfoot belongs
                // to wherever it used to be.
                record->character->RefreshContacts(
                    system.GetDefaultBroadPhaseLayerFilter(record->layer),
                    system.GetDefaultLayerFilter(record->layer),
                    {},
                    {},
                    tempAllocator);
            }

            glm::vec3 characterVelocity(PhysicsCharacterId character) const override {
                const CharacterRecord* record = findCharacter(character);
                return record == nullptr ? glm::vec3{0.f}
                                         : fromJolt(record->character->GetLinearVelocity());
            }

            void setCharacterVelocity(PhysicsCharacterId character, glm::vec3 velocity) override {
                if (CharacterRecord* record = findCharacter(character); record != nullptr) {
                    record->character->SetLinearVelocity(toJolt(velocity));
                }
            }

            CharacterGround characterGround(PhysicsCharacterId character) const override {
                const CharacterRecord* record = findCharacter(character);
                if (record == nullptr) {
                    return CharacterGround{};
                }

                CharacterGround ground{};
                switch (record->character->GetGroundState()) {
                    case JPH::CharacterBase::EGroundState::OnGround:
                        ground.state = CharacterGroundState::grounded;
                        break;
                    case JPH::CharacterBase::EGroundState::OnSteepGround:
                        ground.state = CharacterGroundState::steep;
                        break;
                    // Touching something that cannot hold it is falling, as
                    // far as anything above this cares: what it is touching
                    // still arrives in the normal and the user datum.
                    case JPH::CharacterBase::EGroundState::NotSupported:
                    case JPH::CharacterBase::EGroundState::InAir:
                        ground.state = CharacterGroundState::airborne;
                        break;
                }
                ground.normal = fromJolt(record->character->GetGroundNormal());
                ground.velocity = fromJolt(record->character->GetGroundVelocity());
                ground.userData = record->character->GetGroundUserData();
                return ground;
            }

            void updateCharacter(PhysicsCharacterId character, float deltaSeconds) override {
                CharacterRecord* record = findCharacter(character);
                if (record == nullptr) {
                    return;
                }
                JPH::CharacterVirtual& moving = *record->character;

                // Refuse the part of the velocity that climbs a slope too
                // steep to climb, before moving rather than after: a
                // character allowed to push into a wall first and be pushed
                // back second judders against it.
                moving.SetLinearVelocity(
                    moving.CancelVelocityTowardsSteepSlopes(moving.GetLinearVelocity()));

                JPH::CharacterVirtual::ExtendedUpdateSettings update{};
                update.mStickToFloorStepDown = toJolt(-record->up * record->stickToFloor);
                update.mWalkStairsStepUp = toJolt(record->up * record->stepHeight);

                const glm::vec3 asked = fromJolt(moving.GetLinearVelocity());
                const glm::vec3 before = fromJolt(moving.GetPosition());

                moving.ExtendedUpdate(
                    deltaSeconds,
                    toJolt(worldGravity),
                    update,
                    system.GetDefaultBroadPhaseLayerFilter(record->layer),
                    system.GetDefaultLayerFilter(record->layer),
                    {},
                    {},
                    tempAllocator);

                // What it managed, which Jolt does not work out for itself:
                // the character's velocity is the caller's to own, and a
                // character that walked into a wall would otherwise carry on
                // believing it was running.
                //
                // The plane comes from how far it actually got, so a wall or
                // a slope too steep to climb arrives back as a stop. The axis
                // of up deliberately does not: stepping up a stair and being
                // pulled back down onto a floor are both displacements the
                // character never asked for, and reading them as velocity
                // would fling it off the step or nail it to the ground. What
                // it asked for is kept there instead - with one exception, a
                // rise that did not happen, which is a ceiling.
                const glm::vec3 moved =
                    deltaSeconds > 0.f ? (fromJolt(moving.GetPosition()) - before) / deltaSeconds
                                       : glm::vec3{0.f};
                const float climbed = glm::dot(moved, record->up);
                float vertical = glm::dot(asked, record->up);
                if (vertical > 0.f && climbed <= 0.f) {
                    vertical = 0.f;
                }
                moving.SetLinearVelocity(
                    toJolt(moved - record->up * climbed + record->up * vertical));
            }

            void step(float deltaSeconds) override {
                const JPH::EPhysicsUpdateError error =
                    system.Update(deltaSeconds, 1, &tempAllocator, &jobs);
                if (error != JPH::EPhysicsUpdateError::None) {
                    // Means the world outgrew the limits Init was given -
                    // worth hearing about, because the symptom otherwise is
                    // bodies quietly falling through each other.
                    EGE_ERROR("physics step error {:#x}", static_cast<uint32_t>(error));
                }
            }

            std::vector<ContactEvent> drainContacts() override {
                std::vector<ContactEvent> events = contacts.drain();
                // Jolt's worker threads raise contacts in whatever order the
                // jobs ran; sorted, the same simulation reports the same
                // events in the same order every run, which the determinism
                // guarantee extends to gameplay.
                std::sort(
                    events.begin(), events.end(), [](const ContactEvent& a, const ContactEvent& b) {
                        if (a.userDataA != b.userDataA) {
                            return a.userDataA < b.userDataA;
                        }
                        return a.userDataB < b.userDataB;
                    });
                return events;
            }

            std::vector<SeparationEvent> drainSeparations() override {
                std::vector<SeparationEvent> events = contacts.drainSeparations();
                std::sort(
                    events.begin(),
                    events.end(),
                    [](const SeparationEvent& a, const SeparationEvent& b) {
                        if (a.userDataA != b.userDataA) {
                            return a.userDataA < b.userDataA;
                        }
                        return a.userDataB < b.userDataB;
                    });
                return events;
            }

            std::optional<RaycastHit> raycast(
                glm::vec3 origin, glm::vec3 direction, float maxDistance) const override {
                const glm::vec3 scaled = direction * maxDistance;
                const JPH::RRayCast ray{toJolt(origin), toJolt(scaled)};
                JPH::RayCastResult result;
                if (!system.GetNarrowPhaseQuery().CastRay(ray, result)) {
                    return std::nullopt;
                }

                RaycastHit hit{};
                hit.distance = result.mFraction * glm::length(scaled);
                hit.point = origin + scaled * result.mFraction;
                {
                    JPH::BodyLockRead lock{system.GetBodyLockInterface(), result.mBodyID};
                    if (!lock.Succeeded()) {
                        return std::nullopt;
                    }
                    const JPH::Body& body = lock.GetBody();
                    hit.normal = fromJolt(body.GetWorldSpaceSurfaceNormal(
                        result.mSubShapeID2, ray.GetPointOnRay(result.mFraction)));
                    hit.userData = body.GetUserData();
                }
                return hit;
            }

            glm::vec3 gravity() const override { return worldGravity; }

            const CollisionLayers& collisionLayers() const override { return worldLayers; }

        private:
            // A character, plus the two numbers its move needs that Jolt
            // keeps in the update settings rather than on the character.
            struct CharacterRecord {
                JPH::Ref<JPH::CharacterVirtual> character;
                glm::vec3 up{0.f, 1.f, 0.f};
                float stepHeight = 0.3f;
                float stickToFloor = 0.5f;
                // The object layer it moves as, so a character is filtered by
                // the same matrix as everything else rather than colliding
                // with whatever moves.
                JPH::ObjectLayer layer = 0;
            };

            CharacterRecord* findCharacter(PhysicsCharacterId character) {
                const auto found = characters.find(character);
                return found == characters.end() ? nullptr : &found->second;
            }

            const CharacterRecord* findCharacter(PhysicsCharacterId character) const {
                const auto found = characters.find(character);
                return found == characters.end() ? nullptr : &found->second;
            }

            static glm::vec3 normalisedUp(glm::vec3 up) {
                const float length = glm::length(up);
                return length > 1e-6f ? up / length : glm::vec3{0.f, 1.f, 0.f};
            }

            static JPH::ShapeRefC buildCharacterShape(
                const CharacterSettings& settings, glm::vec3 up) {
                JPH::ShapeSettings::ShapeResult result =
                    JPH::CapsuleShapeSettings{
                        std::max(settings.halfHeight, 0.01f), std::max(settings.radius, 0.01f)}
                        .Create();
                if (result.HasError()) {
                    EGE_ERROR("could not build character shape: {}", result.GetError().c_str());
                    return nullptr;
                }

                // Jolt's capsule stands along Y. A capsule is symmetric about
                // its axis, so a scene whose up is -Y needs no rotation at
                // all - and asking sFromTo for the rotation between opposite
                // vectors is how you get an arbitrary axis and a shape that
                // is subtly not where it says it is.
                const float alignment = glm::dot(up, glm::vec3{0.f, 1.f, 0.f});
                if (std::abs(alignment) > 0.9999f) {
                    return result.Get();
                }

                JPH::ShapeSettings::ShapeResult rotated =
                    JPH::RotatedTranslatedShapeSettings{
                        JPH::Vec3::sZero(),
                        JPH::Quat::sFromTo(JPH::Vec3::sAxisY(), toJolt(up)),
                        result.Get()}
                        .Create();
                if (rotated.HasError()) {
                    EGE_ERROR("could not stand character shape up: {}", rotated.GetError().c_str());
                    return nullptr;
                }
                return rotated.Get();
            }

            static JPH::ShapeRefC buildShape(const BodyShape& shape) {
                JPH::ShapeSettings::ShapeResult result;
                switch (shape.kind) {
                    case BodyShape::Kind::box: {
                        // Jolt refuses boxes thinner than the collision
                        // margin; clamp so a paper-thin floor collider works
                        // instead of failing to build.
                        const JPH::Vec3 halfExtents = toJolt(glm::max(shape.halfExtents, 0.01f));
                        const float margin = std::min(0.05f, halfExtents.ReduceMin() * 0.5f);
                        result = JPH::BoxShapeSettings{halfExtents, margin}.Create();
                        break;
                    }
                    case BodyShape::Kind::sphere:
                        result = JPH::SphereShapeSettings{std::max(shape.radius, 0.01f)}.Create();
                        break;
                    case BodyShape::Kind::capsule:
                        result =
                            JPH::CapsuleShapeSettings{
                                std::max(shape.halfHeight, 0.01f), std::max(shape.radius, 0.01f)}
                                .Create();
                        break;
                }
                if (result.HasError()) {
                    EGE_ERROR("could not build collision shape: {}", result.GetError().c_str());
                    return nullptr;
                }
                if (shape.offset == glm::vec3{0.f}) {
                    return result.Get();
                }
                // An offset collider is the same shape wrapped in a
                // translation, so the body origin can stay on the entity.
                JPH::ShapeSettings::ShapeResult offsetResult =
                    JPH::RotatedTranslatedShapeSettings{
                        toJolt(shape.offset), JPH::Quat::sIdentity(), result.Get()}
                        .Create();
                if (offsetResult.HasError()) {
                    EGE_ERROR(
                        "could not offset collision shape: {}", offsetResult.GetError().c_str());
                    return nullptr;
                }
                return offsetResult.Get();
            }

            // 4 MiB of scratch per step, the figure Jolt's own samples size
            // for scenes far larger than this engine's.
            static constexpr std::size_t tempAllocatorBytes = 4 * 1024 * 1024;

            glm::vec3 worldGravity{0.f, -9.81f, 0.f};
            // Declared before the filter that points at it, because members
            // are destroyed in reverse and a filter outliving its matrix
            // would be a dangling read on the way down.
            CollisionLayers worldLayers{};
            BroadPhaseLayerMap broadPhaseMap{};
            ObjectVsBroadPhaseFilter objectVsBroadPhase{};
            ObjectLayerFilter objectPairs;
            ContactCollector contacts{};
            JPH::TempAllocatorImpl tempAllocator;
            JPH::JobSystemSingleThreaded jobs;
            JPH::PhysicsSystem system{};
            // Characters outlive no step and are keyed by a counter rather
            // than by anything Jolt issues, because a virtual character has
            // no id of its own - it is an object the caller owns.
            std::unordered_map<PhysicsCharacterId, CharacterRecord> characters;
            PhysicsCharacterId nextCharacter = 0;
        };

    }  // namespace

    BodyShape BodyShape::box(glm::vec3 halfExtentsRef, glm::vec3 offsetRef) {
        BodyShape shape{};
        shape.kind = Kind::box;
        shape.halfExtents = halfExtentsRef;
        shape.offset = offsetRef;
        return shape;
    }

    BodyShape BodyShape::sphere(float radiusRef, glm::vec3 offsetRef) {
        BodyShape shape{};
        shape.kind = Kind::sphere;
        shape.radius = radiusRef;
        shape.offset = offsetRef;
        return shape;
    }

    BodyShape BodyShape::capsule(float radiusRef, float halfHeightRef, glm::vec3 offsetRef) {
        BodyShape shape{};
        shape.kind = Kind::capsule;
        shape.radius = radiusRef;
        shape.halfHeight = halfHeightRef;
        shape.offset = offsetRef;
        return shape;
    }

    std::unique_ptr<PhysicsWorld> PhysicsWorld::create(const Settings& settings) {
        ensureJoltRegistered();
        return std::make_unique<JoltPhysicsWorld>(settings);
    }

}  // namespace ege
