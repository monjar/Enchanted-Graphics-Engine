#pragma once

#include "reflect/BuiltinTypes.hpp"
#include "script/Behavior.hpp"

#include <glm/glm.hpp>

namespace ege {

    // The behaviours the demo scene is built from.
    //
    // These live in the engine rather than in a project because there is no
    // project yet - `sandbox/` arrives with the standalone editor. They are
    // written exactly as a project's would be, against nothing but the public
    // API, so that moving them out later is a file move.

    // Constant angular velocity. The replacement for the `Spin` component that
    // stood in for scripting before there was any: the component is gone, and
    // this does the same job as what it was always a placeholder for.
    class Spinner : public Behavior {
    public:
        glm::vec3 anglesPerSecond{0.f, 1.f, 0.f};

        void onFixedTick(float deltaSeconds) override;
    };

    // Circles a point in the XZ plane, keeping its own height.
    class Orbit : public Behavior {
    public:
        glm::vec3 center{0.f};
        float radius = 1.f;
        float radiansPerSecond = 1.f;
        // Where on the circle it starts, so several orbiters do not stack.
        float phase = 0.f;

        void onSpawn() override;
        void onFixedTick(float deltaSeconds) override;

    private:
        float angle = 0.f;
    };

    // Rides up and down about wherever it started. Reads its origin in
    // onSpawn rather than storing one, so it works wherever it is dropped -
    // and so that Stop putting the scene back also puts the ride back.
    class Bobbing : public Behavior {
    public:
        float amplitude = 0.25f;
        float radiansPerSecond = 2.f;

        void onSpawn() override;
        void onFixedTick(float deltaSeconds) override;

    private:
        glm::vec3 origin{0.f};
        float time = 0.f;
    };

    // Drives a CharacterController from the player's input.
    //
    // Movement is relative to where the player is looking rather than to
    // where the body is pointing: forward means forward on the screen, which
    // is what every third-person game means by it and what makes turning a
    // corner one motion rather than two. The look yaw lives here rather than
    // on the camera on purpose - the player looks, and the camera follows.
    class PlayerCharacter : public Behavior {
    public:
        // Radians per pixel of mouse movement.
        float mouseSensitivity = 0.0025f;
        // Radians per second at full deflection, for the things that turn at
        // a rate rather than by an amount: the right stick and the arrow
        // keys.
        float lookSpeed = 2.5f;
        // Where the player is looking, as a yaw about Y. Reflected so a scene
        // can start the player facing somewhere in particular.
        float lookYaw = 0.f;

        void onFixedTick(float deltaSeconds) override;
    };

    // Walks a character round a rectangular circuit centred on wherever it
    // spawned, jumping at the corners.
    //
    // The point is that it drives the character through exactly the fields a
    // player would: the controller cannot tell the difference, which is what
    // makes the recorded demo tour - where there is no player, no gamepad and
    // no mouse - a recording of the same thing a player would get.
    class Patrol : public Behavior {
    public:
        // Half the circuit's size on each axis, in world units. The vertical
        // component is ignored: a patrol walks on whatever floor it finds.
        glm::vec3 extents{2.f, 0.f, 1.5f};
        // How close counts as arrived. Too small and the character circles a
        // corner it can never quite stand on.
        float arriveRadius = 0.4f;
        bool run = false;
        bool jumpAtCorners = true;

        void onSpawn() override;
        void onFixedTick(float deltaSeconds) override;

    private:
        glm::vec3 corner(int index) const;

        glm::vec3 origin{0.f};
        int target = 0;
    };

    // Pushes a dynamic mesh's vertices along their normals by a travelling
    // sine wave. The one behaviour here that exists to prove something rather
    // than to look nice: geometry a script writes every frame.
    class Ripple : public Behavior {
    public:
        float amplitude = 0.08f;
        float wavelength = 0.7f;
        float speed = 1.6f;

        void onSpawn() override;
        void onFixedTick(float deltaSeconds) override;

    private:
        float time = 0.f;
    };

}  // namespace ege

EGE_REFLECT(ege::Spinner)
EGE_FIELD(anglesPerSecond).tooltip("Radians per second about each axis");
EGE_REFLECT_END()

EGE_REFLECT(ege::Orbit)
EGE_FIELD(center).tooltip("Point to circle, in world space");
EGE_FIELD(radius).range(0.f, 20.f);
EGE_FIELD(radiansPerSecond).range(-10.f, 10.f);
EGE_FIELD(phase).range(0.f, 6.2832f).tooltip("Where on the circle it starts");
EGE_REFLECT_END()

EGE_REFLECT(ege::Bobbing)
EGE_FIELD(amplitude).range(0.f, 5.f);
EGE_FIELD(radiansPerSecond).range(0.f, 20.f);
EGE_REFLECT_END()

EGE_REFLECT(ege::Ripple)
EGE_FIELD(amplitude).range(0.f, 1.f);
EGE_FIELD(wavelength).range(0.05f, 5.f);
EGE_FIELD(speed).range(-10.f, 10.f);
EGE_REFLECT_END()

EGE_REFLECT(ege::PlayerCharacter)
EGE_FIELD(mouseSensitivity).range(0.0001f, 0.02f).tooltip("Radians per pixel of mouse movement");
EGE_FIELD(lookSpeed).range(0.f, 10.f).tooltip("Radians per second for the stick and arrow keys");
EGE_FIELD(lookYaw).range(-3.1416f, 3.1416f).tooltip("Where the player is looking, about Y");
EGE_REFLECT_END()

EGE_REFLECT(ege::Patrol)
EGE_FIELD(extents).tooltip("Half the circuit's size on each axis; Y is ignored");
EGE_FIELD(arriveRadius).range(0.05f, 3.f).tooltip("How close to a corner counts as arrived");
EGE_FIELD(run);
EGE_FIELD(jumpAtCorners);
EGE_REFLECT_END()
