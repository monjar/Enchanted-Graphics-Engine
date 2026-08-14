#pragma once

#include "scene/World.hpp"

#include <cstddef>
#include <deque>
#include <string>
#include <vector>

namespace ege {

    // Undo and redo, as whole-scene mementos.
    //
    // Storing the state before each change rather than a description of the
    // change is the unfashionable choice and, here, the right one. The engine
    // already has a fast, complete, reflection-driven serializer, so a
    // memento is one call and is correct by construction for every kind of
    // edit - a dragged slider, a deleted subtree, a component attached, a
    // reparent - without a command class per operation, each of which is
    // somewhere a bug can hide. Inverse commands become worth their cost when
    // scenes are large enough that copying one is not; a demo scene is eight
    // kilobytes.
    //
    // The price is paid honestly and stated in one place: restoring respawns
    // every entity, so handles held across an undo go stale. The editor
    // re-finds its selection by name afterwards.
    class UndoStack {
    public:
        // Deep enough to cover a working session, bounded so that a long one
        // does not grow without limit.
        static constexpr std::size_t maxDepth = 64;

        // Records the state to come back to. Call *before* making a change -
        // after it, the state being recorded is the one the change produced.
        void record(const World& world, std::string label);

        // The same, from a memento captured earlier. What a dragged slider
        // needs: the state to return to is the one from before the drag
        // started, not the one that exists by the time the drag ends.
        void recordCaptured(std::string scene, std::string label);

        bool canUndo() const { return !past.empty(); }

        bool canRedo() const { return !future.empty(); }

        // What the next undo or redo would reverse, for the menu to show.
        const std::string& undoLabel() const;
        const std::string& redoLabel() const;

        void undo(World& world);
        void redo(World& world);

        void clear();

        std::size_t depth() const { return past.size(); }

    private:
        struct Entry {
            std::string label;
            std::string scene;
        };

        static std::string capture(const World& world);
        static void restore(World& world, const std::string& scene);

        // Oldest at the front, so exceeding the depth drops the change the
        // user is least likely to still want.
        std::deque<Entry> past;
        std::vector<Entry> future;
    };

}  // namespace ege
