#include "editor/UndoStack.hpp"

#include "core/Log.hpp"
#include "scene/SceneSerializer.hpp"

#include <stdexcept>
#include <utility>

namespace ege {

    namespace {

        const std::string emptyLabel;

    }  // namespace

    std::string UndoStack::capture(const World& world) {
        try {
            return SceneSerializer::toString(world);
        } catch (const std::exception& error) {
            EGE_ERROR("cannot record an undo step: {}", error.what());
            return {};
        }
    }

    void UndoStack::restore(World& world, const std::string& scene) {
        try {
            SceneSerializer::fromString(world, scene);
        } catch (const std::exception& error) {
            EGE_ERROR("undo failed to restore the scene: {}", error.what());
        }
    }

    void UndoStack::record(const World& world, std::string label) {
        recordCaptured(capture(world), std::move(label));
    }

    void UndoStack::recordCaptured(std::string scene, std::string label) {
        if (scene.empty()) {
            // Recording nothing would leave an entry that undoes to an empty
            // world, which is worse than having no entry at all.
            return;
        }

        past.push_back(Entry{std::move(label), std::move(scene)});
        while (past.size() > maxDepth) {
            past.pop_front();
        }
        // A new change makes the redo branch unreachable, which is what every
        // editor does and what users expect.
        future.clear();
    }

    const std::string& UndoStack::undoLabel() const {
        return past.empty() ? emptyLabel : past.back().label;
    }

    const std::string& UndoStack::redoLabel() const {
        return future.empty() ? emptyLabel : future.back().label;
    }

    void UndoStack::undo(World& world) {
        if (past.empty()) {
            return;
        }

        Entry entry = std::move(past.back());
        past.pop_back();

        // The state being left becomes the redo target, under the same label:
        // undoing "delete Floor" and redoing it are the same change seen from
        // either side.
        future.push_back(Entry{entry.label, capture(world)});
        restore(world, entry.scene);
        EGE_DEBUG("undo: {}", entry.label);
    }

    void UndoStack::redo(World& world) {
        if (future.empty()) {
            return;
        }

        Entry entry = std::move(future.back());
        future.pop_back();

        past.push_back(Entry{entry.label, capture(world)});
        restore(world, entry.scene);
        EGE_DEBUG("redo: {}", entry.label);
    }

    void UndoStack::clear() {
        past.clear();
        future.clear();
    }

}  // namespace ege
