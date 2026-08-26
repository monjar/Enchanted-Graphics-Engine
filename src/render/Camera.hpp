#pragma once

#include <glm/glm.hpp>

namespace ege {
    class Camera {
    public:
        void setOrthographicProjection(
            float left, float right, float top, float bottom, float nearPlane, float farPlane);

        void setPerspectiveProjection(float fovY, float aspect, float nearPlane, float farPlane);

        void setViewDirection(
            glm::vec3 position, glm::vec3 direction, glm::vec3 up = glm::vec3{0.f, -1.f, 0.f});
        void setViewTarget(
            glm::vec3 position, glm::vec3 target, glm::vec3 up = glm::vec3{0.f, -1.f, 0.f});
        void setViewYXZ(glm::vec3 position, glm::vec3 rotation);

        const glm::mat4& getProjection() const { return projectionMatrix; }

        const glm::mat4& getView() const { return viewMatrix; }

    private:
        glm::mat4 projectionMatrix{1.f};
        glm::mat4 viewMatrix{1.f};
    };

    // Which way a viewer with these Tait-Bryan angles is looking, and which
    // way is up for it.
    //
    // The same two columns setViewYXZ builds its matrix from, pulled out
    // because something other than the renderer wants them now: the audio
    // listener sits where the camera sits and hears along the same axes, and
    // two places deriving a forward vector from angles is two places for the
    // rotation order to be wrong in.
    glm::vec3 forwardFromAngles(glm::vec3 rotation);

    glm::vec3 upFromAngles(glm::vec3 rotation);

}  // namespace ege