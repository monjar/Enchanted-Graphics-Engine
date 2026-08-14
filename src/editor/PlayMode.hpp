#pragma once

#include "scene/World.hpp"

#include <string>

namespace ege {

    // Running the scene without leaving the editor, and getting it back.
    //
    // Play snapshots the world, Stop restores it. That is the whole contract,
    // and it is the reason this could not exist before the asset database: a
    // snapshot is a scene file, and until a scene file could describe what a
    // MeshRenderer draws, Stop would have restored a world with no geometry
    // in it. The blocked feature was the honest measure of the missing one.
    //
    // Snapshotting through the serializer rather than by copying pools is
    // deliberate. It is slower, and it means anything a scene file cannot
    // describe does not survive Play - which is a property worth having
    // visible rather than hidden, because it is the same set of things that
    // will not survive a save.
    class PlayMode {
    public:
        enum class State { editing, playing, paused };

        State state() const { return current; }

        bool isEditing() const { return current == State::editing; }

        bool isPlaying() const { return current == State::playing; }

        bool isPaused() const { return current == State::paused; }

        // Takes the snapshot and starts running. Does nothing if already
        // playing, so that a second Play cannot overwrite the state Stop is
        // supposed to return to.
        void play(const World& world);

        void pause();

        void resume();

        // Advances exactly one fixed step and pauses again. Starting from
        // edit mode plays first, which is what a step button is expected to
        // do when nothing is running yet.
        void step(const World& world);

        // Restores the snapshot. Does nothing while editing.
        void stop(World& world);

        // Whether gameplay should advance this fixed step, consuming a pending
        // single-step request. Called once per fixed step, not per frame.
        bool consumeTick();

        // Bytes of scene held by the snapshot, for the stats panel to show.
        std::size_t snapshotSize() const { return snapshot.size(); }

    private:
        State current = State::editing;
        std::string snapshot;
        bool stepPending = false;
    };

}  // namespace ege
