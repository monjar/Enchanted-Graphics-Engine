// Watching a directory for changes.
//
// The properties that matter are about *not* reporting things: the first look
// establishes what "before" means rather than reporting the whole tree as new,
// and a poll inside the interval does nothing at all - a watcher called every
// frame must not walk the tree every frame.

#include "core/FileWatcher.hpp"

#include <doctest/doctest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>

using ege::FileWatcher;

namespace {

    namespace fs = std::filesystem;

    class TemporaryTree {
    public:
        explicit TemporaryTree(const std::string& label)
            : root{fs::temp_directory_path() / ("ege-watch-" + label)} {
            std::error_code errorCode;
            fs::remove_all(root, errorCode);
            fs::create_directories(root, errorCode);
        }

        ~TemporaryTree() {
            std::error_code errorCode;
            fs::remove_all(root, errorCode);
        }

        TemporaryTree(const TemporaryTree&) = delete;
        TemporaryTree& operator=(const TemporaryTree&) = delete;

        const fs::path& path() const { return root; }

        void write(const fs::path& relative, const std::string& contents) const {
            const fs::path full = root / relative;
            std::error_code errorCode;
            fs::create_directories(full.parent_path(), errorCode);
            std::ofstream file{full};
            file << contents;
        }

        // File clocks are coarse - a second's granularity is not unusual - so
        // a modification has to be stamped rather than waited for. Writing and
        // then setting the time is how a test stays fast and still looks like
        // a real edit to the watcher.
        void touch(const fs::path& relative, int secondsLater) const {
            const fs::path full = root / relative;
            std::error_code errorCode;
            const auto now = fs::last_write_time(full, errorCode);
            fs::last_write_time(full, now + std::chrono::seconds{secondsLater}, errorCode);
        }

    private:
        fs::path root;
    };

    // Zero interval, so every poll walks: these tests are about what is
    // reported, not about how often.
    FileWatcher makeWatcher(const TemporaryTree& tree) {
        return FileWatcher{tree.path(), std::chrono::milliseconds{0}};
    }

}  // namespace

TEST_CASE("the first look reports nothing") {
    TemporaryTree tree{"first"};
    tree.write("a.txt", "one");
    tree.write("nested/b.txt", "two");

    FileWatcher watcher = makeWatcher(tree);

    CHECK(watcher.poll().empty());
    // And still nothing, with nothing changed in between.
    CHECK(watcher.poll().empty());
}

TEST_CASE("a modified file is reported once") {
    TemporaryTree tree{"modify"};
    tree.write("a.txt", "one");

    FileWatcher watcher = makeWatcher(tree);
    watcher.poll();

    tree.touch("a.txt", 5);
    const std::vector<FileWatcher::Event> events = watcher.poll();

    REQUIRE(events.size() == 1);
    CHECK(events.front().change == FileWatcher::Change::modified);
    CHECK(events.front().path.filename() == "a.txt");

    // Reported once, not every poll until something else happens.
    CHECK(watcher.poll().empty());
}

TEST_CASE("added and removed files are reported") {
    TemporaryTree tree{"addremove"};
    tree.write("a.txt", "one");

    FileWatcher watcher = makeWatcher(tree);
    watcher.poll();

    tree.write("nested/b.txt", "two");
    std::vector<FileWatcher::Event> events = watcher.poll();
    REQUIRE(events.size() == 1);
    CHECK(events.front().change == FileWatcher::Change::added);
    CHECK(events.front().path.filename() == "b.txt");

    std::error_code errorCode;
    fs::remove(tree.path() / "a.txt", errorCode);
    events = watcher.poll();
    REQUIRE(events.size() == 1);
    CHECK(events.front().change == FileWatcher::Change::removed);
    CHECK(events.front().path.filename() == "a.txt");
}

TEST_CASE("a poll inside the interval does nothing") {
    TemporaryTree tree{"interval"};
    tree.write("a.txt", "one");

    FileWatcher watcher{tree.path(), std::chrono::milliseconds{60000}};
    // Even the priming walk waits for the interval, so the very first poll
    // reports nothing and the watcher is still unprimed.
    CHECK(watcher.poll().empty());

    tree.write("b.txt", "two");
    CHECK(watcher.poll().empty());
}

TEST_CASE("a missing directory is not an error") {
    // The project folder can be deleted out from under a running editor, and
    // that should stop the reporting rather than the engine.
    TemporaryTree tree{"missing"};
    tree.write("a.txt", "one");

    FileWatcher watcher = makeWatcher(tree);
    watcher.poll();

    std::error_code errorCode;
    fs::remove_all(tree.path(), errorCode);

    CHECK(watcher.poll().empty());
}

TEST_CASE("resetting forgets what was seen") {
    TemporaryTree tree{"reset"};
    tree.write("a.txt", "one");

    FileWatcher watcher = makeWatcher(tree);
    watcher.poll();
    watcher.reset();

    // The next poll is a first look again, so an existing file is not new.
    CHECK(watcher.poll().empty());
    tree.touch("a.txt", 5);
    CHECK(watcher.poll().size() == 1);
}
