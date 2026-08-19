#version 450

// One level of the hierarchical depth buffer, from the level below it.
//
// Each output texel is the farthest depth of the source texels it covers, so
// that every level answers "what is the worst case anywhere under here?" for a
// larger piece of the screen than the last. That is what lets a whole object's
// screen rectangle be bounded in a handful of lookups.
//
// The same shader builds the first level from the depth buffer itself: a depth
// image sampled through an ordinary sampler hands back its depth in the red
// channel, so nothing here needs to know which it is reading.
//
// See src/render/OcclusionCulling.hpp, where the levels above this are built
// on the CPU by the same rule and the rule is tested.

layout(location = 0) in vec2 fragUv;

layout(location = 0) out float outDepth;

layout(set = 0, binding = 0) uniform sampler2D sourceLevel;

layout(push_constant) uniform Push {
    ivec2 sourceSize;
} push;

void main() {
    ivec2 target = ivec2(gl_FragCoord.xy);
    ivec2 base = target * 2;

    // Two source texels per axis, except along an axis whose source size is
    // odd, where the last texel would otherwise belong to no parent at all.
    // Three overlapping is harmless - a parent may claim a farther maximum
    // than it strictly covers, and claiming too far only ever prevents a cull.
    // Claiming too near culls something that can be seen.
    int spanX = (push.sourceSize.x % 2) != 0 ? 3 : 2;
    int spanY = (push.sourceSize.y % 2) != 0 ? 3 : 2;

    float farthest = 0.0;
    for (int y = 0; y < spanY; y++) {
        for (int x = 0; x < spanX; x++) {
            ivec2 at = min(base + ivec2(x, y), push.sourceSize - 1);
            farthest = max(farthest, texelFetch(sourceLevel, at, 0).r);
        }
    }

    outDepth = farthest;
}
