#pragma once

#include "anim/AnimationSampling.hpp"
#include "scene/World.hpp"

#include <glm/glm.hpp>

#include <vector>

namespace ege {

    // Advances every animator and turns its pose into palette matrices.
    //
    // Runs in the editor as well as in play - an animation is how a rigged
    // mesh looks, not something that happens to it, and a character frozen
    // in bind pose until Play is a character being authored blind. What Play
    // owns is gameplay; a clip advancing is the same kind of motion as a
    // material's ripple.
    class AnimationSystem {
    public:
        // How many matrices one frame's palette holds, across every animated
        // entity. 256 is sixteen characters of sixteen joints or four of
        // sixty-four; an entity past the budget keeps last frame's base and
        // holds its pose, and the shortfall is logged once.
        static constexpr uint32_t paletteCapacity = 256;

        // Samples every animator at its current time, advances the playing
        // ones by `deltaSeconds`, and packs their skinning matrices into
        // `palette` end to end, writing each animator's base back into its
        // component. The palette is sized to exactly what was written.
        void update(World& world, float deltaSeconds, std::vector<glm::mat4>& palette);

    private:
        // Scratch, kept across frames so steady-state animation allocates
        // nothing.
        std::vector<JointPose> pose;
        std::vector<glm::mat4> globals;
        std::vector<glm::mat4> skins;
        bool warnedFull = false;
    };

}  // namespace ege
