#pragma once

#include "scene/Components.hpp"

#include <cstddef>
#include <vector>

namespace ege {

    // A scripted camera move, for showing the engine with nobody's hand on the
    // mouse.
    //
    // This exists because a renderer is hard to demonstrate in a still image
    // and impossible to demonstrate in a paragraph. Most of what the engine
    // does only becomes visible when the camera moves: a mirror sphere is just
    // a coloured ball until its reflection slides, a shadow is just a dark
    // patch until it swings, and bloom is just brightness until a highlight
    // crosses the frame.
    //
    // Waypoints rather than a spline. A spline would move more smoothly and
    // would need tangents chosen by hand for every point; eased interpolation
    // between held poses is what a product tour actually looks like, and it
    // makes each shot something that can be aimed.
    class DemoTour {
    public:
        struct Shot {
            glm::vec3 position{0.f};
            // YXZ Euler angles, matching Transform and the camera controller.
            glm::vec3 rotation{0.f};
            // Time spent travelling here from the previous shot, and time held
            // once it arrives. The first shot's travel time is ignored - there
            // is nowhere to travel from.
            float travelSeconds = 3.f;
            float holdSeconds = 1.f;
        };

        // The tour the demo scene is built around.
        static DemoTour demoScene();

        explicit DemoTour(std::vector<Shot> shots);

        // Advances by one frame and writes the camera pose. Returns false once
        // the tour has run out, which is how the demo knows to close.
        bool advance(float deltaSeconds, Transform& viewer);

        void restart();

        float elapsed() const { return time; }

        float duration() const { return total; }

    private:
        std::vector<Shot> path;
        float time = 0.f;
        float total = 0.f;
    };

}  // namespace ege
