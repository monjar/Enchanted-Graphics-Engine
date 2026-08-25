// The animation arithmetic, and the import that feeds it.
//
// Everything here is checked against values derived by hand - a lerp worked
// out on paper, a 90-degree quaternion written from the definition - never
// against the code being tested. The import test builds a complete rigged
// glTF byte by byte and pushes it through the real parser, because the
// numbers a rig arrives with are exactly the numbers skinning will multiply
// every vertex by, and a wrong inverse bind produces a plausible monster.

#include "anim/AnimationSampling.hpp"
#include "assets/GltfLoader.hpp"

#include <doctest/doctest.h>

#include <cmath>
#include <cstring>
#include <string>
#include <vector>

using ege::AnimationChannel;
using ege::AnimationClip;
using ege::AnimationPath;
using ege::Joint;
using ege::JointPose;
using ege::Skeleton;

namespace {

    // Two joints in a chain: a root at (0,1,0), a tip one unit above it.
    Skeleton chainSkeleton() {
        Skeleton skeleton;
        skeleton.joints.resize(2);
        skeleton.joints[0].name = "root";
        skeleton.joints[0].parent = -1;
        skeleton.joints[0].rest.translation = {0.f, 1.f, 0.f};
        skeleton.joints[1].name = "tip";
        skeleton.joints[1].parent = 0;
        skeleton.joints[1].rest.translation = {0.f, 2.f, 0.f};
        return skeleton;
    }

    glm::quat aboutY(float degrees) {
        return glm::angleAxis(glm::radians(degrees), glm::vec3{0.f, 1.f, 0.f});
    }

    AnimationClip slideClip() {
        AnimationClip clip;
        clip.name = "slide";
        clip.duration = 2.f;
        AnimationChannel channel;
        channel.joint = 1;
        channel.path = AnimationPath::translation;
        channel.times = {0.f, 2.f};
        channel.values = {{0.f, 2.f, 0.f, 0.f}, {4.f, 2.f, 0.f, 0.f}};
        clip.channels.push_back(std::move(channel));
        return clip;
    }

}  // namespace

TEST_CASE("sampling interpolates between keys and rests elsewhere") {
    const Skeleton skeleton = chainSkeleton();
    const AnimationClip clip = slideClip();

    std::vector<JointPose> pose;
    ege::samplePose(skeleton, clip, 0.5f, false, pose);

    // The animated joint is a quarter of the way along its slide.
    CHECK(pose[1].translation.x == doctest::Approx(1.f));
    CHECK(pose[1].translation.y == doctest::Approx(2.f));
    // The joint no channel names keeps its rest pose - which is what stops
    // a clip that animates one arm collapsing the rest of the body.
    CHECK(pose[0].translation.y == doctest::Approx(1.f));
}

TEST_CASE("time clamps when not looping and wraps when it does") {
    CHECK(ege::clipTime(-1.f, 2.f, false) == doctest::Approx(0.f));
    CHECK(ege::clipTime(5.f, 2.f, false) == doctest::Approx(2.f));
    CHECK(ege::clipTime(2.5f, 2.f, true) == doctest::Approx(0.5f));
    CHECK(ege::clipTime(-0.5f, 2.f, true) == doctest::Approx(1.5f));
    // A clip with no length reads its start whatever is asked of it.
    CHECK(ege::clipTime(3.f, 0.f, true) == doctest::Approx(0.f));
}

TEST_CASE("a stepped channel holds its key instead of interpolating") {
    const Skeleton skeleton = chainSkeleton();
    AnimationClip clip = slideClip();
    clip.channels[0].stepped = true;

    std::vector<JointPose> pose;
    ege::samplePose(skeleton, clip, 1.9f, false, pose);
    CHECK(pose[1].translation.x == doctest::Approx(0.f));
    ege::samplePose(skeleton, clip, 2.f, false, pose);
    CHECK(pose[1].translation.x == doctest::Approx(4.f));
}

TEST_CASE("rotation keys slerp along the shorter arc") {
    const Skeleton skeleton = chainSkeleton();
    AnimationClip clip;
    clip.duration = 1.f;
    AnimationChannel channel;
    channel.joint = 0;
    channel.path = AnimationPath::rotation;
    channel.times = {0.f, 1.f};
    const glm::quat quarter = aboutY(90.f);
    channel.values = {{0.f, 0.f, 0.f, 1.f}, {quarter.x, quarter.y, quarter.z, quarter.w}};
    clip.channels.push_back(std::move(channel));

    std::vector<JointPose> pose;
    ege::samplePose(skeleton, clip, 0.5f, false, pose);

    // Halfway from identity to a quarter turn is an eighth of a turn.
    const float angle = glm::degrees(glm::angle(pose[0].rotation));
    CHECK(angle == doctest::Approx(45.f).epsilon(0.001));
    CHECK(glm::axis(pose[0].rotation).y == doctest::Approx(1.f).epsilon(0.001));
}

TEST_CASE("blending is the midpoint it claims to be") {
    std::vector<JointPose> from(1);
    std::vector<JointPose> to(1);
    from[0].translation = {0.f, 0.f, 0.f};
    to[0].translation = {2.f, 0.f, 0.f};
    to[0].rotation = aboutY(90.f);
    to[0].scale = {3.f, 3.f, 3.f};

    std::vector<JointPose> mid;
    ege::blendPoses(from, to, 0.5f, mid);
    CHECK(mid[0].translation.x == doctest::Approx(1.f));
    CHECK(glm::degrees(glm::angle(mid[0].rotation)) == doctest::Approx(45.f).epsilon(0.001));
    CHECK(mid[0].scale.x == doctest::Approx(2.f));
}

TEST_CASE("global transforms accumulate down the chain") {
    const Skeleton skeleton = chainSkeleton();
    std::vector<JointPose> pose(2);
    pose[0] = skeleton.joints[0].rest;
    pose[1] = skeleton.joints[1].rest;
    // Turn the root a quarter turn about Y; the tip rides along.
    pose[0].rotation = aboutY(90.f);

    std::vector<glm::mat4> globals;
    ege::globalTransforms(skeleton, pose, globals);

    // The tip sits two above the root whichever way the root faces...
    const glm::vec3 tip{globals[1] * glm::vec4{0.f, 0.f, 0.f, 1.f}};
    CHECK(tip.x == doctest::Approx(0.f).epsilon(0.001));
    CHECK(tip.y == doctest::Approx(3.f));
    // ...and a point one unit ahead of the tip swings from +z to +x, which
    // is what a quarter turn about +Y does to +Z.
    const glm::vec3 ahead{globals[1] * glm::vec4{0.f, 0.f, 1.f, 1.f}};
    CHECK(ahead.x == doctest::Approx(1.f).epsilon(0.001));
    CHECK(ahead.z == doctest::Approx(0.f).epsilon(0.001));
}

TEST_CASE("skinning matrices cancel to identity in bind pose") {
    // The defining property: a rig posed exactly as it was bound moves no
    // vertex at all. Any failure here deforms every model everywhere.
    Skeleton skeleton = chainSkeleton();
    std::vector<JointPose> pose(2);
    pose[0] = skeleton.joints[0].rest;
    pose[1] = skeleton.joints[1].rest;

    std::vector<glm::mat4> globals;
    ege::globalTransforms(skeleton, pose, globals);
    // Bind the joints where the rest pose puts them.
    skeleton.joints[0].inverseBind = glm::inverse(globals[0]);
    skeleton.joints[1].inverseBind = glm::inverse(globals[1]);

    std::vector<glm::mat4> skins;
    ege::skinningMatrices(skeleton, globals, skins);
    const glm::vec3 vertex{1.f, 2.5f, -0.5f};
    const glm::vec3 skinned{skins[1] * glm::vec4{vertex, 1.f}};
    CHECK(skinned.x == doctest::Approx(vertex.x));
    CHECK(skinned.y == doctest::Approx(vertex.y));
    CHECK(skinned.z == doctest::Approx(vertex.z));
}

// ---------------------------------------------------------------------------
// The import.
// ---------------------------------------------------------------------------

namespace {

    void appendFloats(std::vector<unsigned char>& buffer, std::initializer_list<float> values) {
        for (float value : values) {
            unsigned char bytes[sizeof(float)];
            std::memcpy(bytes, &value, sizeof(float));
            buffer.insert(buffer.end(), bytes, bytes + sizeof(float));
        }
    }

    void appendShorts(std::vector<unsigned char>& buffer, std::initializer_list<uint16_t> values) {
        for (uint16_t value : values) {
            unsigned char bytes[sizeof(uint16_t)];
            std::memcpy(bytes, &value, sizeof(uint16_t));
            buffer.insert(buffer.end(), bytes, bytes + sizeof(uint16_t));
        }
    }

    std::string base64(const std::vector<unsigned char>& bytes) {
        static const char alphabet[] =
            "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        std::string out;
        out.reserve((bytes.size() + 2) / 3 * 4);
        for (std::size_t i = 0; i < bytes.size(); i += 3) {
            const unsigned a = bytes[i];
            const unsigned b = i + 1 < bytes.size() ? bytes[i + 1] : 0u;
            const unsigned c = i + 2 < bytes.size() ? bytes[i + 2] : 0u;
            out.push_back(alphabet[a >> 2]);
            out.push_back(alphabet[((a & 0x3u) << 4) | (b >> 4)]);
            out.push_back(i + 1 < bytes.size() ? alphabet[((b & 0xFu) << 2) | (c >> 6)] : '=');
            out.push_back(i + 2 < bytes.size() ? alphabet[c & 0x3Fu] : '=');
        }
        return out;
    }

    // A complete rigged file: two joints, a clip sliding the tip and turning
    // the root, and a three-vertex skinned triangle. The joints are listed
    // in the skin *child first*, so the test proves the importer reorders -
    // a file that happened to be parent-first would pass with no reordering
    // at all.
    std::string riggedGltf() {
        std::vector<unsigned char> buffer;
        // offset 0: key times.
        appendFloats(buffer, {0.f, 1.f});
        // offset 8: tip translations, resting to slid.
        appendFloats(buffer, {0.f, 2.f, 0.f, 3.f, 2.f, 0.f});
        // offset 32: root rotations, identity to a quarter turn about Y
        // (xyzw, as glTF stores them).
        const float half = 0.70710678f;
        appendFloats(buffer, {0.f, 0.f, 0.f, 1.f, 0.f, half, 0.f, half});
        // offset 64: inverse binds in the skin's (child-first) joint order:
        // the tip's undoes its bind height of 3, the root's its height of 1.
        appendFloats(
            buffer,
            {1.f, 0.f, 0.f, 0.f, 0.f, 1.f, 0.f, 0.f, 0.f, 0.f, 1.f, 0.f, 0.f, -3.f, 0.f, 1.f});
        appendFloats(
            buffer,
            {1.f, 0.f, 0.f, 0.f, 0.f, 1.f, 0.f, 0.f, 0.f, 0.f, 1.f, 0.f, 0.f, -1.f, 0.f, 1.f});
        // offset 192: three positions.
        appendFloats(buffer, {0.f, 0.f, 0.f, 1.f, 0.f, 0.f, 0.f, 2.f, 0.f});
        // offset 228: joints, in the skin's own numbering: 0 is the tip.
        appendShorts(buffer, {1, 0, 0, 0});
        appendShorts(buffer, {0, 0, 0, 0});
        appendShorts(buffer, {0, 1, 0, 0});
        // offset 252: weights; the last vertex's are unnormalised on purpose.
        appendFloats(buffer, {1.f, 0.f, 0.f, 0.f});
        appendFloats(buffer, {1.f, 0.f, 0.f, 0.f});
        appendFloats(buffer, {0.25f, 0.25f, 0.f, 0.f});
        REQUIRE(buffer.size() == 300);

        return std::string{R"({
            "asset": {"version": "2.0"},
            "scene": 0,
            "scenes": [{"nodes": [0, 2]}],
            "nodes": [
                {"name": "root", "translation": [0, 1, 0], "children": [1]},
                {"name": "tip", "translation": [0, 2, 0]},
                {"name": "body", "mesh": 0, "skin": 0}
            ],
            "skins": [{"name": "rig", "joints": [1, 0], "inverseBindMatrices": 3}],
            "meshes": [{"name": "triangle", "primitives": [{
                "attributes": {"POSITION": 4, "JOINTS_0": 5, "WEIGHTS_0": 6}}]}],
            "animations": [{
                "name": "wave",
                "samplers": [
                    {"input": 0, "output": 1, "interpolation": "LINEAR"},
                    {"input": 0, "output": 2, "interpolation": "LINEAR"}
                ],
                "channels": [
                    {"sampler": 0, "target": {"node": 1, "path": "translation"}},
                    {"sampler": 1, "target": {"node": 0, "path": "rotation"}}
                ]
            }],
            "accessors": [
                {"bufferView": 0, "componentType": 5126, "count": 2, "type": "SCALAR",
                 "min": [0.0], "max": [1.0]},
                {"bufferView": 1, "componentType": 5126, "count": 2, "type": "VEC3"},
                {"bufferView": 2, "componentType": 5126, "count": 2, "type": "VEC4"},
                {"bufferView": 3, "componentType": 5126, "count": 2, "type": "MAT4"},
                {"bufferView": 4, "componentType": 5126, "count": 3, "type": "VEC3"},
                {"bufferView": 5, "componentType": 5123, "count": 3, "type": "VEC4"},
                {"bufferView": 6, "componentType": 5126, "count": 3, "type": "VEC4"}
            ],
            "bufferViews": [
                {"buffer": 0, "byteOffset": 0, "byteLength": 8},
                {"buffer": 0, "byteOffset": 8, "byteLength": 24},
                {"buffer": 0, "byteOffset": 32, "byteLength": 32},
                {"buffer": 0, "byteOffset": 64, "byteLength": 128},
                {"buffer": 0, "byteOffset": 192, "byteLength": 36},
                {"buffer": 0, "byteOffset": 228, "byteLength": 24},
                {"buffer": 0, "byteOffset": 252, "byteLength": 48}
            ],
            "buffers": [{"byteLength": 300,
                "uri": "data:application/octet-stream;base64,)"} +
               base64(buffer) + R"("}]
        })";
    }

}  // namespace

TEST_CASE("a rigged glTF arrives with its hierarchy in skeleton order") {
    const std::string file = riggedGltf();
    const ege::GltfSceneData scene = ege::gltf::parseMemory(file.data(), file.size());

    REQUIRE(scene.skins.size() == 1);
    const ege::Skeleton& skeleton = scene.skins[0].skeleton;
    REQUIRE(skeleton.joints.size() == 2);

    // The file listed the tip before the root; the importer put the parent
    // first regardless, and the invariant every sweep relies on holds.
    CHECK(skeleton.joints[0].name == "root");
    CHECK(skeleton.joints[0].parent == -1);
    CHECK(skeleton.joints[1].name == "tip");
    CHECK(skeleton.joints[1].parent == 0);

    // Rest poses from the nodes, inverse binds from the accessor - each
    // matrix following its joint through the reorder.
    CHECK(skeleton.joints[0].rest.translation.y == doctest::Approx(1.f));
    CHECK(skeleton.joints[1].rest.translation.y == doctest::Approx(2.f));
    CHECK(skeleton.joints[0].inverseBind[3][1] == doctest::Approx(-1.f));
    CHECK(skeleton.joints[1].inverseBind[3][1] == doctest::Approx(-3.f));

    CHECK(scene.nodes[2].skin == 0);
}

TEST_CASE("clips arrive resolved against the skeleton") {
    const std::string file = riggedGltf();
    const ege::GltfSceneData scene = ege::gltf::parseMemory(file.data(), file.size());

    REQUIRE(scene.clips.size() == 1);
    const AnimationClip& clip = scene.clips[0];
    CHECK(clip.name == "wave");
    CHECK(clip.duration == doctest::Approx(1.f));
    REQUIRE(clip.channels.size() == 2);

    // And they sample: at the clip's midpoint the tip - skeleton joint 1 -
    // is half way through its slide, and the root has turned an eighth.
    std::vector<JointPose> pose;
    ege::samplePose(scene.skins[0].skeleton, clip, 0.5f, false, pose);
    CHECK(pose[1].translation.x == doctest::Approx(1.5f));
    CHECK(glm::degrees(glm::angle(pose[0].rotation)) == doctest::Approx(45.f).epsilon(0.001));
}

TEST_CASE("vertex joints arrive remapped and weights renormalised") {
    const std::string file = riggedGltf();
    const ege::GltfSceneData scene = ege::gltf::parseMemory(file.data(), file.size());

    const ege::GltfPrimitiveData& primitive = scene.meshes[0].primitives[0];
    REQUIRE(primitive.joints.size() == 3);
    REQUIRE(primitive.weights.size() == 3);

    // The file's joint 0 was the tip and 1 the root; after the remap the
    // indices mean skeleton order, where the root is 0.
    CHECK(primitive.joints[0] == glm::uvec4{0, 1, 1, 1});
    CHECK(primitive.joints[1] == glm::uvec4{1, 1, 1, 1});
    CHECK(primitive.joints[2] == glm::uvec4{1, 0, 1, 1});

    // The vertex whose weights summed to a half sums to one now, with the
    // proportions kept.
    CHECK(primitive.weights[2].x == doctest::Approx(0.5f));
    CHECK(primitive.weights[2].y == doctest::Approx(0.5f));
}
