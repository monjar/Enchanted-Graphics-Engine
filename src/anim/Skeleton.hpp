#pragma once

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <string>
#include <vector>

namespace ege {

    // One joint's local pose: the three parts kept apart rather than a
    // matrix, because everything animation does to a pose - keyframe
    // interpolation, blending between clips - is defined on the parts and
    // wrong on the product. Two matrices averaged component-wise shear;
    // two rotations slerped stay rotations.
    struct JointPose {
        glm::vec3 translation{0.f};
        glm::quat rotation{1.f, 0.f, 0.f, 0.f};
        glm::vec3 scale{1.f};
    };

    struct Joint {
        std::string name;
        // Index into the skeleton's joints, and always smaller than this
        // joint's own index: parents precede children, which is what lets a
        // global-pose pass be one forward sweep. The importer reorders
        // whatever the file held to make this true. -1 marks a root.
        int parent = -1;
        // Bind-space to joint-space: the matrix that takes a vertex from
        // where the mesh was modelled to where this joint sees it. The
        // skinning matrix is the joint's global transform times this.
        glm::mat4 inverseBind{1.f};
        // The joint's local pose when nothing animates it. Sampling starts
        // from here, so a clip that animates only an arm leaves the rest of
        // the body posed rather than collapsed to identity.
        JointPose rest{};
    };

    // A rig: joints in parent-before-child order. Plain data, no behaviour -
    // the arithmetic that animates one lives in AnimationSampling, where the
    // tests can reach it.
    struct Skeleton {
        std::vector<Joint> joints;
    };

}  // namespace ege
