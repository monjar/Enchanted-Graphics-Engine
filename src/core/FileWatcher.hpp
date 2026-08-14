#pragma once

#include <chrono>
#include <filesystem>
#include <string>
#include <unordered_map>
#include <vector>

namespace ege {

    // Notices when files under a directory change.
    //
    // Polling modification times rather than subscribing to inotify,
    // ReadDirectoryChangesW and FSEvents. Three platform APIs with three sets
    // of quirks, against one loop that costs a stat per file on an interval -
    // for a project directory that is nothing, and the version that works
    // everywhere on the first try is worth more than the one that is elegant
    // on Linux.
    //
    // The same watcher serves both users: assets, whose reload is here now,
    // and scripts, whose reload needs a compiler as well.
    class FileWatcher {
    public:
        enum class Change { added, modified, removed };

        struct Event {
            std::filesystem::path path;
            Change change = Change::modified;
        };

        // `interval` is the floor on how often the tree is actually walked.
        // poll() may be called every frame; it returns immediately until the
        // interval has passed.
        FileWatcher(std::filesystem::path root, std::chrono::milliseconds interval);

        // What changed since the previous walk. Empty when the interval has
        // not elapsed, and empty on the first call - the first walk records
        // what is there rather than reporting the whole tree as new.
        std::vector<Event> poll();

        // Forgets what it has seen, so the next poll reports nothing and
        // starts again from the tree as it stands.
        void reset();

        const std::filesystem::path& root() const { return watchedRoot; }

    private:
        using Clock = std::chrono::steady_clock;

        std::filesystem::path watchedRoot;
        std::chrono::milliseconds pollInterval;
        Clock::time_point lastPoll{};
        bool primed = false;
        std::unordered_map<std::string, std::filesystem::file_time_type> seen;
    };

}  // namespace ege
