// glTF parsing.
//
// The importer is split into a CPU parse and a GPU instantiate exactly so
// this file can exist: everything about reading the format - accessors,
// node transforms, material factors, hierarchy - is checked here against an
// embedded test asset, with no device anywhere. The asset is a two-node
// scene (a transformed parent, a child carrying a triangle) written as a
// self-contained glTF string with its buffer in a base64 data URI.

#include "assets/GltfLoader.hpp"

#include <glm/gtc/constants.hpp>
#include <glm/gtc/epsilon.hpp>

#include <doctest/doctest.h>

#include <cstring>
#include <stdexcept>

using ege::GltfSceneData;

namespace {

    // Buffer layout: 3 vec3 float positions (36 bytes), then 3 uint16
    // indices padded to 44 total. Positions (0,0,0), (2,0,0), (0,2,0).
    constexpr const char* triangleGltf = R"({
  "asset": {"version": "2.0"},
  "scene": 0,
  "scenes": [{"nodes": [0]}],
  "nodes": [
    {
      "name": "parent",
      "translation": [1, 2, 3],
      "children": [1]
    },
    {
      "name": "child",
      "rotation": [0, 0.7071068, 0, 0.7071068],
      "scale": [2, 2, 2],
      "mesh": 0
    }
  ],
  "meshes": [
    {
      "name": "triangle",
      "primitives": [
        {
          "attributes": {"POSITION": 0},
          "indices": 1,
          "material": 0
        }
      ]
    }
  ],
  "materials": [
    {
      "name": "testMaterial",
      "pbrMetallicRoughness": {
        "baseColorFactor": [0.8, 0.2, 0.1, 1.0],
        "metallicFactor": 0.9,
        "roughnessFactor": 0.3
      },
      "emissiveFactor": [0.1, 0.2, 0.3],
      "alphaMode": "MASK",
      "alphaCutoff": 0.4
    }
  ],
  "accessors": [
    {
      "bufferView": 0,
      "componentType": 5126,
      "count": 3,
      "type": "VEC3",
      "min": [0, 0, 0],
      "max": [2, 2, 0]
    },
    {
      "bufferView": 1,
      "componentType": 5123,
      "count": 3,
      "type": "SCALAR"
    }
  ],
  "bufferViews": [
    {"buffer": 0, "byteOffset": 0, "byteLength": 36},
    {"buffer": 0, "byteOffset": 36, "byteLength": 6}
  ],
  "buffers": [
    {
      "byteLength": 44,
      "uri": "data:application/octet-stream;base64,AAAAAAAAAAAAAAAAAAAAQAAAAAAAAAAAAAAAAAAAAEAAAAAAAAABAAIAAAA="
    }
  ]
})";

    GltfSceneData parseTriangle() {
        return ege::gltf::parseMemory(triangleGltf, std::strlen(triangleGltf));
    }

}  // namespace

TEST_CASE("a minimal glTF parses into one mesh with one primitive") {
    GltfSceneData scene = parseTriangle();

    REQUIRE(scene.meshes.size() == 1);
    CHECK(scene.meshes[0].name == "triangle");
    REQUIRE(scene.meshes[0].primitives.size() == 1);

    const auto& primitive = scene.meshes[0].primitives[0];
    REQUIRE(primitive.vertices.size() == 3);
    REQUIRE(primitive.indices.size() == 3);
    CHECK(primitive.material == 0);

    CHECK(primitive.vertices[1].position.x == doctest::Approx(2.f));
    CHECK(primitive.vertices[2].position.y == doctest::Approx(2.f));
    CHECK(primitive.indices[0] == 0);
    CHECK(primitive.indices[2] == 2);

    // No COLOR_0 in the file: vertex color defaults to white, because the
    // shader multiplies by it.
    CHECK(primitive.vertices[0].color == glm::vec3{1.f, 1.f, 1.f});
}

TEST_CASE("missing normals are generated from the winding") {
    GltfSceneData scene = parseTriangle();
    const auto& primitive = scene.meshes[0].primitives[0];

    // Triangle in the XY plane wound (0,0)->(2,0)->(0,2): the generated
    // normal points along +Z.
    for (const auto& vertex : primitive.vertices) {
        CHECK(vertex.normal.z == doctest::Approx(1.f));
    }
}

TEST_CASE("material factors survive the trip") {
    GltfSceneData scene = parseTriangle();

    REQUIRE(scene.materials.size() == 1);
    const auto& material = scene.materials[0];
    CHECK(material.name == "testMaterial");
    CHECK(material.properties.baseColorFactor.r == doctest::Approx(0.8f));
    CHECK(material.properties.metallicFactor == doctest::Approx(0.9f));
    CHECK(material.properties.roughnessFactor == doctest::Approx(0.3f));
    CHECK(material.properties.emissiveFactor.b == doctest::Approx(0.3f));
    CHECK(material.properties.alphaCutoff == doctest::Approx(0.4f));

    // No textures in the file: every slot keeps the fallback.
    CHECK(material.baseColorImage == -1);
    CHECK(material.normalImage == -1);
    CHECK(scene.images.empty());
}

TEST_CASE("the node hierarchy and transforms come through") {
    GltfSceneData scene = parseTriangle();

    REQUIRE(scene.nodes.size() == 2);
    REQUIRE(scene.rootNodes.size() == 1);

    const auto& parent = scene.nodes[scene.rootNodes[0]];
    CHECK(parent.name == "parent");
    CHECK(parent.mesh == -1);
    CHECK(parent.transform.translation == glm::vec3{1.f, 2.f, 3.f});
    REQUIRE(parent.children.size() == 1);

    const auto& child = scene.nodes[parent.children[0]];
    CHECK(child.name == "child");
    CHECK(child.mesh == 0);
    CHECK(child.transform.scale.x == doctest::Approx(2.f));

    // The quaternion (0, sin45, 0, cos45) is a quarter turn about Y; the
    // decomposition into the engine's YXZ Euler convention must agree.
    CHECK(child.transform.rotation.y == doctest::Approx(glm::half_pi<float>()).epsilon(1e-3));
    CHECK(child.transform.rotation.x == doctest::Approx(0.f).epsilon(1e-3));
    CHECK(child.transform.rotation.z == doctest::Approx(0.f).epsilon(1e-3));
}

TEST_CASE("a decomposed transform recomposes to the same matrix") {
    GltfSceneData scene = parseTriangle();
    const auto& child = scene.nodes[scene.nodes[scene.rootNodes[0]].children[0]];

    // Rebuild through Transform::mat4 and check the rotation landed where
    // the quaternion said: a quarter turn about Y maps +X to -Z, scaled.
    const glm::mat4 recomposed = child.transform.mat4();
    const glm::vec4 mappedX = recomposed * glm::vec4{1.f, 0.f, 0.f, 0.f};
    CHECK(mappedX.x == doctest::Approx(0.f).epsilon(1e-3));
    CHECK(mappedX.z == doctest::Approx(-2.f).epsilon(1e-3));
}

TEST_CASE("garbage input is refused with an explanation") {
    const char* junk = "{ not gltf at all";
    CHECK_THROWS_AS(ege::gltf::parseMemory(junk, std::strlen(junk)), std::runtime_error);
}
