#include "editor/PlayMode.hpp"

#include "core/Log.hpp"
#include "scene/SceneSerializer.hpp"

#include <stdexcept>

namespace ege {

    void PlayMode::play(const World& world) {
        if (current != State::editing) {
            current = State::playing;
            return;
        }

        try {
            snapshot = SceneSerializer::toString(world);
        } catch (const std::exception& error) {
            // Refusing to start beats starting with no way back.
            EGE_ERROR("cannot enter play mode: snapshotting the scene failed: {}", error.what());
            return;
        }

        current = State::playing;
        EGE_INFO("Play: {} entities snapshotted, {} bytes", world.entityCount(), snapshot.size());
    }

    void PlayMode::pause() {
        if (current == State::playing) {
            current = State::paused;
        }
    }

    void PlayMode::resume() {
        if (current == State::paused) {
            current = State::playing;
        }
    }

    void PlayMode::step(const World& world) {
        if (current == State::editing) {
            play(world);
        }
        if (current == State::editing) {
            // play() refused; there is nothing to step.
            return;
        }
        current = State::paused;
        stepPending = true;
    }

    void PlayMode::stop(World& world) {
        if (current == State::editing) {
            return;
        }

        current = State::editing;
        stepPending = false;

        try {
            SceneSerializer::fromString(world, snapshot);
            EGE_INFO("Stop: scene restored, {} entities", world.entityCount());
        } catch (const std::exception& error) {
            // The world is now whatever the failed load left behind, which is
            // worth being loud about: the user's scene is the thing at stake.
            EGE_ERROR("restoring the scene failed: {}", error.what());
        }
        snapshot.clear();
    }

    bool PlayMode::consumeTick() {
        if (current == State::playing) {
            return true;
        }
        if (current == State::paused && stepPending) {
            stepPending = false;
            return true;
        }
        return false;
    }

}  // namespace ege
