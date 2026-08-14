#include "core/FileWatcher.hpp"

#include <utility>

namespace ege {

    FileWatcher::FileWatcher(std::filesystem::path root, std::chrono::milliseconds interval)
        : watchedRoot{std::move(root)}, pollInterval{interval}, lastPoll{Clock::now()} {}

    void FileWatcher::reset() {
        seen.clear();
        primed = false;
    }

    std::vector<FileWatcher::Event> FileWatcher::poll() {
        const Clock::time_point now = Clock::now();
        if (primed && now - lastPoll < pollInterval) {
            return {};
        }
        lastPoll = now;

        std::error_code errorCode;
        if (!std::filesystem::is_directory(watchedRoot, errorCode)) {
            return {};
        }

        std::unordered_map<std::string, std::filesystem::file_time_type> current;
        std::vector<Event> events;

        for (const std::filesystem::directory_entry& entry :
             std::filesystem::recursive_directory_iterator{watchedRoot, errorCode}) {
            if (!entry.is_regular_file(errorCode)) {
                continue;
            }

            const std::filesystem::file_time_type written =
                std::filesystem::last_write_time(entry.path(), errorCode);
            if (errorCode) {
                // A file being written to right now can fail to stat. Skipping
                // it means it is reported on the next poll instead, which is
                // exactly what should happen: a half-written file is not a
                // change worth acting on.
                errorCode.clear();
                continue;
            }

            std::string key = entry.path().string();
            const auto previous = seen.find(key);
            if (previous == seen.end()) {
                if (primed) {
                    events.push_back(Event{entry.path(), Change::added});
                }
            } else if (previous->second != written) {
                events.push_back(Event{entry.path(), Change::modified});
            }
            current.emplace(std::move(key), written);
        }

        if (primed) {
            for (const auto& [path, time] : seen) {
                if (current.find(path) == current.end()) {
                    events.push_back(Event{std::filesystem::path{path}, Change::removed});
                }
            }
        }

        seen = std::move(current);
        // The first walk is what "before" means; reporting the whole tree as
        // new would make start-up look like a change to everything.
        primed = true;
        return events;
    }

}  // namespace ege
