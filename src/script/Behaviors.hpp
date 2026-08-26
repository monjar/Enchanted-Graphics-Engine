#pragma once

#include "reflect/BuiltinTypes.hpp"
#include "scene/Prefab.hpp"
#include "script/Behavior.hpp"
#include "script/GameEvents.hpp"

#include <glm/glm.hpp>

#include <string>

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

    // Picks the clip a character's own motion calls for, and plays it at the
    // rate the ground demands.
    //
    // The whole of the state machine v0.5 asked for, which turns out to be
    // four rules rather than a graph editor: airborne is a jump, standing
    // still is an idle, moving is a walk, moving fast is a run. What makes it
    // small is that the character controller already worked out the answer -
    // `grounded` and `planarSpeed` are exactly the two questions - so this
    // reads state rather than tracking any.
    //
    // Attached to the entity carrying the SkeletalAnimator, which for an
    // imported model is a child of the one carrying the controller; the
    // controller is looked for up the hierarchy.
    class CharacterAnimation : public Behavior {
    public:
        int idleClip = 0;
        int walkClip = 1;
        int runClip = 2;
        int jumpClip = 3;

        // The ground speeds the walk and run clips were authored at. A clip
        // played at the speed it was drawn for has its feet planted; played
        // at any other, they skate.
        float walkSpeed = 0.9f;
        float runSpeed = 1.8f;
        // Below this the character is standing still rather than creeping.
        float idleSpeed = 0.08f;
        // How long a change of mind takes.
        float fadeSeconds = 0.15f;

        void onFixedTick(float deltaSeconds) override;
    };

    // A door that slides open while something stands on its plate.
    //
    // Attached to the plate, not to the door: the plate is the thing that
    // knows, and what it knows is a count. Two crates and a player standing
    // on it is three arrivals and three departures, and a door that opened on
    // the first and shut on the first departure would shut under whoever was
    // still standing there.
    //
    // The door itself is a kinematic body, so it pushes what is in the way
    // rather than passing through it - which is also what stops a player
    // closing a door on themselves and ending up inside it.
    class PressurePlate : public Behavior {
    public:
        // The entity to move, by name. A name rather than a handle because a
        // handle means nothing in the next run, and this has to survive being
        // saved.
        std::string door;
        // How far the door travels from where it started, and how fast.
        glm::vec3 opening{0.f, -0.9f, 0.f};
        float speed = 1.6f;

        void onSpawn() override;
        void onTriggerEnter(Entity other) override;
        void onTriggerExit(Entity other) override;
        void onFixedTick(float deltaSeconds) override;

    private:
        glm::vec3 closed{0.f};
        // How many things are standing on it, and how far open the door is.
        int occupants = 0;
        float openness = 0.f;
    };

    // Stamps out a prefab when something enters the trigger it is attached
    // to.
    //
    // The whole of what a prefab is for, in one behaviour: naming an asset
    // and asking for a copy of it. Nothing here knows what is in the prefab -
    // a crate, a pickup, a monster - which is the difference between spawning
    // and constructing.
    class Spawner : public Behavior {
    public:
        PrefabRef prefab;
        // Where the copy appears, relative to this entity.
        glm::vec3 offset{0.f, -1.f, 0.f};
        // The most it will ever make. A spawner that runs forever fills the
        // world, and the world is what the spawner is standing in.
        int limit = 4;
        // Seconds before it will answer again, so a character standing in the
        // volume does not become a fountain.
        float cooldown = 1.5f;

        void onSpawn() override;
        void onTriggerEnter(Entity other) override;
        void onFixedTick(float deltaSeconds) override;

    private:
        int made = 0;
        float ready = 0.f;
    };

    // A thing that is collected by being touched.
    //
    // Attached to whatever the prefab spawns. It says what happened and
    // removes itself, and it knows nothing at all about scores, doors or how
    // many of it there are - which is what lets the same prefab be a pickup
    // in one level and scenery in another.
    class Pickup : public Behavior {
    public:
        // Only this collision layer collects it, by the same reasoning a
        // trigger has an `only`: a crate bumping into a coin has not picked
        // it up.
        std::string collectedBy = "Character";

        void onContact(const Contact& contact) override;
    };

    // Counts pickups and declares the level over.
    //
    // The middle of the chain, and the only thing that knows there is a
    // chain: it hears `PickupCollected` from things it has never met, and
    // when the last one lands it waits a beat and raises `LevelComplete`. The
    // beat is a timer rather than a frame counter, so it is the same length
    // of *simulated* time however the frame rate wanders.
    class Goal : public Behavior {
    public:
        // How many make a level. Reached from below rather than counted down,
        // so a level that spawns more than it needs still ends.
        int needed = 3;
        // Seconds between the last pickup and the celebration, so the two
        // read as cause and effect rather than as one event.
        float celebrateAfter = 1.f;

        void onSpawn() override;

        int collected() const { return count; }

    private:
        int count = 0;
        bool finished = false;
    };

    // Slides an entity somewhere else, once, when the level is won.
    //
    // Attached to the door. It has never heard of a pickup: it knows a level
    // can be complete and what it does about that, which is the whole of what
    // an event buys.
    class OpenOnLevelComplete : public Behavior {
    public:
        glm::vec3 opening{0.f, -0.9f, 0.f};
        float speed = 1.2f;

        void onSpawn() override;
        void onFixedTick(float deltaSeconds) override;

    private:
        glm::vec3 closed{0.f};
        float openness = 0.f;
        bool opening_ = false;
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

EGE_REFLECT(ege::CharacterAnimation)
EGE_FIELD(idleClip).range(0.f, 32.f);
EGE_FIELD(walkClip).range(0.f, 32.f);
EGE_FIELD(runClip).range(0.f, 32.f);
EGE_FIELD(jumpClip).range(0.f, 32.f);
EGE_FIELD(walkSpeed).range(0.f, 20.f).tooltip("The ground speed the walk clip was drawn for");
EGE_FIELD(runSpeed).range(0.f, 40.f).tooltip("The ground speed the run clip was drawn for");
EGE_FIELD(idleSpeed).range(0.f, 2.f).tooltip("Below this it is standing still");
EGE_FIELD(fadeSeconds).range(0.f, 1.f).tooltip("How long a change of clip takes");
EGE_REFLECT_END()

EGE_REFLECT(ege::Spawner)
EGE_FIELD(prefab).tooltip("The scene fragment to stamp out");
EGE_FIELD(offset).tooltip("Where the copy appears, relative to this entity");
EGE_FIELD(limit).range(0.f, 64.f).tooltip("The most it will ever make");
EGE_FIELD(cooldown).range(0.f, 30.f).tooltip("Seconds before it answers again");
EGE_REFLECT_END()

EGE_REFLECT(ege::Pickup)
EGE_FIELD(collectedBy).tooltip("Only this collision layer collects it");
EGE_REFLECT_END()

EGE_REFLECT(ege::Goal)
EGE_FIELD(needed).range(1.f, 64.f).tooltip("How many pickups make a level");
EGE_FIELD(celebrateAfter).range(0.f, 10.f).tooltip("Seconds before the celebration");
EGE_REFLECT_END()

EGE_REFLECT(ege::OpenOnLevelComplete)
EGE_FIELD(opening).tooltip("How far it travels from where it started");
EGE_FIELD(speed).range(0.f, 20.f).tooltip("Units per second");
EGE_REFLECT_END()

EGE_REFLECT(ege::PressurePlate)
EGE_FIELD(door).tooltip("The entity this plate opens, by name");
EGE_FIELD(opening).tooltip("How far the door travels from where it started");
EGE_FIELD(speed).range(0.f, 20.f).tooltip("Units per second the door moves");
EGE_REFLECT_END()

EGE_REFLECT(ege::Patrol)
EGE_FIELD(extents).tooltip("Half the circuit's size on each axis; Y is ignored");
EGE_FIELD(arriveRadius).range(0.05f, 3.f).tooltip("How close to a corner counts as arrived");
EGE_FIELD(run);
EGE_FIELD(jumpAtCorners);
EGE_REFLECT_END()
