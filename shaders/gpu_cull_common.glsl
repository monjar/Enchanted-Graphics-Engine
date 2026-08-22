// What the two culling dispatches share: the frame's inputs and the append
// that compacts a visible object into its batch's window.
//
// This is the running copy of src/render/GpuCulling.cpp, which is the copy
// the tests pin - kept in step by hand, the same arrangement the cluster
// grid and the SSAO kernel live under. If the two drift, objects vanish while
// plainly on screen, which is the failure occlusion culling always produces
// when it is wrong.

// One candidate, as prepare() wrote it: a world-space bounding sphere, which
// draw command the object belongs to, and where that batch's window in the
// compacted instance buffer begins. The window start is carried here so this
// shader never reads a command's firstInstance field, whose position depends
// on whether the batch draws indexed.
struct CullInput {
    vec4 sphere;
    uint batch;
    uint batchFirst;
    uint pad0;
    uint pad1;
};

// Mirrors GpuInstance: the two matrices the vertex shaders read.
struct CullInstance {
    mat4 modelMatrix;
    mat4 normalMatrix;
};

layout(set = 0, binding = 0) uniform CullUbo {
    mat4 view;
    mat4 projection;
    // x, y: the projection's own depth terms, so a sphere's nearest depth is
    // depthTerms.x + depthTerms.y / viewZ. z: the near plane. w: nonzero
    // while the occlusion test is on; zero passes everything, which is the
    // culling-off switch without a second pipeline.
    vec4 depthTerms;
    // x: candidates this frame. y: batches. z: where the late pass's half of
    // the instance buffer begins.
    uvec4 counts;
    // The bound pyramid levels' sizes, finest first.
    ivec4 levelExtents[4];
} cull;

layout(std430, set = 0, binding = 1) readonly buffer CullInputs {
    CullInput at[];
} inputs;

layout(std430, set = 0, binding = 2) readonly buffer Candidates {
    CullInstance at[];
} candidates;

layout(std430, set = 0, binding = 3) writeonly buffer InstancesOut {
    CullInstance at[];
} instancesOut;

// The frame's draw commands as raw words, because two layouts share the
// buffer - five words for an indexed draw, four for a plain one - and the
// one field this shader touches sits at word 1 in both.
layout(std430, set = 0, binding = 4) buffer Commands {
    uint words[];
} commands;

// One word per candidate slot: whether the object was visible the last time
// the late pass ruled on it. Written by the late pass, read by the early one
// a frame later. Keyed by draw-list position, which is not guaranteed stable
// across frames - and does not need to be: a wrong guess here draws a real
// object's depth early or defers one to the late pass, and the late pass
// rules on everything against real depth either way. Instability costs a
// little efficiency for a frame and can never cost a missing object.
layout(std430, set = 0, binding = 5) buffer Visibility {
    uint flags[];
} visibility;

// Compacts candidate i into its batch's draw. `lateRegion` is zero for the
// early pass and counts.z for the late one, whose windows sit a whole
// buffer along so neither pass needs the other's count.
void appendInstance(uint i, uint commandSlot, uint lateRegion) {
    uint slot = atomicAdd(commands.words[commandSlot * 5u + 1u], 1u);
    uint destination = lateRegion + inputs.at[i].batchFirst + slot;
    instancesOut.at[destination] = candidates.at[i];
}
