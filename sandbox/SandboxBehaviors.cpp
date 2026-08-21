// The sandbox project's behaviours.
//
// Written against nothing but the engine's public headers, and compiled into a
// module the engine loads at runtime rather than into the engine itself. That
// is the only difference between these and the ones in src/script/Behaviors -
// and it is the difference that makes them editable while the engine runs.

#include "reflect/BuiltinTypes.hpp"
#include "scene/Components.hpp"
#include "scene/Hierarchy.hpp"
#include "script/Behavior.hpp"
#include "script/BehaviorRegistry.hpp"

#include <cmath>

namespace sandbox {

    // Breathes: scales an entity up and down about the size it was found at.
    //
    // The size is worked out in onSpawn rather than stored, which is what lets
    // a reload land on its feet - the replacement instance asks the world how
    // big the thing is now instead of inheriting a number from an instance
    // that no longer exists.
    class Pulse : public ege::Behavior {
    public:
        float amount = 0.25f;
        float radiansPerSecond = 2.5f;

        void onSpawn() override {
            if (const ege::Transform* transform = self().find<ege::Transform>()) {
                restingScale = transform->scale;
            }
            phase = 0.f;
        }

        void onFixedTick(float deltaSeconds) override {
            phase += radiansPerSecond * deltaSeconds;

            ege::Transform* transform = self().find<ege::Transform>();
            if (transform == nullptr) {
                return;
            }
            transform->scale = restingScale * (1.f + amount * std::sin(phase));

            // Anything that writes a Transform has to say so: world matrices
            // are cached behind a dirty flag, and a behaviour writing one is
            // in exactly the position the inspector is.
            ege::hierarchy::markDirty(world(), self().id());
        }

    private:
        glm::vec3 restingScale{1.f};
        float phase = 0.f;
    };

}  // namespace sandbox

EGE_REFLECT(sandbox::Pulse)
EGE_FIELD(amount).range(0.f, 1.f).tooltip("How far it swells, as a fraction of its resting size");
EGE_FIELD(radiansPerSecond).range(0.f, 12.f).tooltip("How fast it breathes");
EGE_REFLECT_END()

EGE_BEHAVIOR(sandbox::Pulse)
