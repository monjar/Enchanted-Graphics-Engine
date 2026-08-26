#include "script/Behaviors.hpp"

#include "anim/SkeletalAnimator.hpp"
#include "physics/CharacterMotion.hpp"
#include "physics/PhysicsComponents.hpp"
#include "platform/Input.hpp"
#include "render/DynamicMesh.hpp"
#include "scene/Components.hpp"
#include "scene/Hierarchy.hpp"
#include "script/BehaviorRegistry.hpp"

#include <glm/gtc/constants.hpp>

#include <cmath>

namespace ege {

    namespace {

        // Every behaviour that moves something has to say so: world matrices
        // are cached behind a dirty flag, and a script writing a Transform is
        // in exactly the position the inspector is.
        void moved(Behavior& behavior) {
            hierarchy::markDirty(behavior.world(), behavior.self().id());
        }

    }  // namespace

    void Spinner::onFixedTick(float deltaSeconds) {
        Transform* transform = self().find<Transform>();
        if (transform == nullptr) {
            return;
        }
        transform->rotation += anglesPerSecond * deltaSeconds;
        moved(*this);
    }

    void Orbit::onSpawn() {
        angle = phase;
    }

    void Orbit::onFixedTick(float deltaSeconds) {
        Transform* transform = self().find<Transform>();
        if (transform == nullptr) {
            return;
        }
        angle += radiansPerSecond * deltaSeconds;
        transform->translation.x = center.x + std::cos(angle) * radius;
        transform->translation.z = center.z + std::sin(angle) * radius;
        moved(*this);
    }

    void Bobbing::onSpawn() {
        // Read rather than authored: the behaviour works wherever it is
        // dropped, and Stop putting the scene back also puts the ride back.
        if (const Transform* transform = self().find<Transform>()) {
            origin = transform->translation;
        }
        time = 0.f;
    }

    void Bobbing::onFixedTick(float deltaSeconds) {
        Transform* transform = self().find<Transform>();
        if (transform == nullptr) {
            return;
        }
        time += deltaSeconds;
        transform->translation = origin;
        transform->translation.y = origin.y + std::sin(time * radiansPerSecond) * amplitude;
        moved(*this);
    }

    void PlayerCharacter::onFixedTick(float deltaSeconds) {
        CharacterController* controller = self().find<CharacterController>();
        Input* input = world().input();
        if (controller == nullptr || input == nullptr) {
            return;
        }

        // Turning while the cursor is captured, which is when the mouse is
        // the player's rather than the editor's. Without the check, dragging
        // a slider in the inspector also spins the character.
        if (input->cursorMode() != CursorMode::Normal) {
            lookYaw += input->mouseDelta().x * mouseSensitivity;
        }
        // The right stick turns as well, at a rate per second rather than per
        // pixel: a stick held over is a steady turn, where a mouse moved is a
        // fixed amount of turn.
        lookYaw += (input->axis("LookLeft", "LookRight") + input->rightStick().x) * lookSpeed *
                   deltaSeconds;
        lookYaw = std::fmod(lookYaw, glm::two_pi<float>());

        // The same forward the rest of the engine builds from a yaw, so
        // "forward" means the same thing to the character as to the camera.
        const glm::vec3 forward{std::sin(lookYaw), 0.f, std::cos(lookYaw)};
        // Keyboard and stick added rather than chosen between, so a player
        // can put a hand on either at any moment; the length is clamped
        // downstream, which is what stops the two adding up to double speed.
        const glm::vec2 stick =
            glm::vec2{
                input->axis("MoveLeft", "MoveRight"), input->axis("MoveBackward", "MoveForward")} +
            input->leftStick();

        // Up is the world's, not the entity's: the demo scene's is -Y, and
        // the plane a character walks in is the one perpendicular to gravity.
        // In edit mode there is no physics world to ask, and the conventional
        // up is as good an answer as any for a character that is not moving.
        const glm::vec3 up = world().physics() != nullptr
                                 ? upFromGravity(world().physics()->gravity())
                                 : glm::vec3{0.f, 1.f, 0.f};
        controller->move = moveDirection(stick, forward, up);
        controller->run = input->isActionDown("Run");
        controller->jumpHeld = input->isActionDown("Jump");
        if (input->wasActionPressed("Jump")) {
            controller->jump = true;
        }
    }

    void CharacterAnimation::onFixedTick(float) {
        SkeletalAnimator* animator = self().find<SkeletalAnimator>();
        if (animator == nullptr) {
            return;
        }

        // The controller is on this entity or on an ancestor: an imported
        // model arrives as a subtree, and the thing that walks is the root it
        // was parented to.
        const CharacterController* controller = nullptr;
        for (EntityId entity = self().id(); !entity.isNull() && controller == nullptr;
             entity = hierarchy::parentOf(world(), entity)) {
            controller = world().find<CharacterController>(entity);
        }
        if (controller == nullptr) {
            return;
        }

        if (!controller->grounded) {
            // From the top: a jump is a thing that happens once, and picking
            // it up mid-clip would play the landing on the way up.
            animator->play(jumpClip, fadeSeconds, true);
            animator->loop = false;
            animator->speed = 1.f;
            return;
        }

        animator->loop = true;
        if (controller->planarSpeed <= idleSpeed) {
            animator->play(idleClip, fadeSeconds, true);
            animator->speed = 1.f;
            return;
        }

        // Walk below the midpoint of the two authored speeds, run above it.
        // A single crossing point rather than a band: hysteresis would be the
        // right answer if the speed oscillated, and it does not - the
        // controller accelerates smoothly through the middle and the fade
        // covers the moment.
        const bool running = controller->planarSpeed > (walkSpeed + runSpeed) * 0.5f;
        const float authored = running ? runSpeed : walkSpeed;
        // Phase carried across, because a walk becoming a run is one stride
        // turning into a faster stride.
        animator->play(running ? runClip : walkClip, fadeSeconds, false);
        animator->speed = authored > 0.f ? controller->planarSpeed / authored : 1.f;
    }

    void Patrol::onSpawn() {
        const Transform* transform = self().find<Transform>();
        origin = transform != nullptr ? transform->translation : glm::vec3{0.f};
        target = 0;
    }

    glm::vec3 Patrol::corner(int index) const {
        // Four corners in order, so consecutive ones share an edge and the
        // circuit is a rectangle rather than an X.
        static constexpr float signs[4][2] = {{1.f, 1.f}, {-1.f, 1.f}, {-1.f, -1.f}, {1.f, -1.f}};
        const int wrapped = ((index % 4) + 4) % 4;
        return origin +
               glm::vec3{extents.x * signs[wrapped][0], 0.f, extents.z * signs[wrapped][1]};
    }

    void Patrol::onFixedTick(float) {
        CharacterController* controller = self().find<CharacterController>();
        const Transform* transform = self().find<Transform>();
        if (controller == nullptr || transform == nullptr) {
            return;
        }

        const glm::vec3 destination = corner(target);
        // Distance in the plane only: a corner on a floor the character has
        // walked up to is still the corner it was aiming for.
        const glm::vec2 gap{
            destination.x - transform->translation.x, destination.z - transform->translation.z};
        if (glm::length(gap) <= arriveRadius) {
            target = (target + 1) % 4;
            if (jumpAtCorners) {
                controller->jump = true;
            }
            controller->move = glm::vec3{0.f};
            return;
        }

        const glm::vec2 heading = glm::normalize(gap);
        controller->move = glm::vec3{heading.x, 0.f, heading.y};
        controller->run = run;
    }

    void Ripple::onSpawn() {
        time = 0.f;
    }

    void Ripple::onFixedTick(float deltaSeconds) {
        DynamicMesh* mesh = self().find<DynamicMesh>();
        if (mesh == nullptr || mesh->vertices.empty()) {
            return;
        }

        time += deltaSeconds;

        // Height is a pure function of position and elapsed time, so the
        // surface never drifts and Stop restoring the clock restores the
        // shape. A behaviour that integrated its vertices instead would come
        // back from a snapshot looking like wherever it happened to be.
        const float frequency = 6.283185f / wavelength;
        for (Model::Vertex& vertex : mesh->vertices) {
            const float distance = std::sqrt(
                vertex.position.x * vertex.position.x + vertex.position.z * vertex.position.z);
            vertex.position.y = std::sin(distance * frequency - time * speed) * amplitude;
        }

        mesh->recalculateNormals();
        mesh->markDirty();
    }

}  // namespace ege

EGE_BEHAVIOR(ege::Spinner)
EGE_BEHAVIOR(ege::Orbit)
EGE_BEHAVIOR(ege::Bobbing)
EGE_BEHAVIOR(ege::Ripple)
EGE_BEHAVIOR(ege::PlayerCharacter)
EGE_BEHAVIOR(ege::Patrol)
EGE_BEHAVIOR(ege::CharacterAnimation)
