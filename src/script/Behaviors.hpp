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
