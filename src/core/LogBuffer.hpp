#pragma once

#include <cstddef>
#include <deque>
#include <mutex>
#include <string>
#include <vector>

namespace ege {

    // Severity, as the rest of the engine sees it.
    //
    // Deliberately not spdlog's enum: the console is a consumer of the log, not
    // of the logging library, and nothing above core/ should have to know which
    // backend is installed for the engine to swap it.
    enum class LogLevel { trace, debug, info, warn, error, critical };

    const char* logLevelName(LogLevel level);

    // The recent log, kept in memory so that something other than a terminal
    // can show it.
    //
    // A bounded ring rather than the whole history: a long session logs far
    // more than anyone scrolls back through, and an unbounded buffer turns a
    // chatty frame loop into a memory leak. Records are pushed from whichever
    // thread logged - the job system's workers included - so every access is
    // under the lock, and readers take a copy rather than holding it while a
    // panel is drawn.
    class LogBuffer {
    public:
        struct Record {
            LogLevel level = LogLevel::info;
            std::string logger;
            std::string message;
        };

        static constexpr std::size_t capacity = 1024;

        static LogBuffer& instance();

        void push(Record record);

        std::vector<Record> snapshot() const;

        void clear();

        // Bumped on every push, so a reader can tell "nothing new" from "the
        // same thing again" without comparing contents.
        std::uint64_t revision() const;

    private:
        mutable std::mutex mutex;
        std::deque<Record> records;
        std::uint64_t writes = 0;
    };

}  // namespace ege
