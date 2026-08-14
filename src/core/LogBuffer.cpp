#include "core/LogBuffer.hpp"

#include <cstdint>

namespace ege {

    const char* logLevelName(LogLevel level) {
        switch (level) {
            case LogLevel::trace:
                return "trace";
            case LogLevel::debug:
                return "debug";
            case LogLevel::info:
                return "info";
            case LogLevel::warn:
                return "warn";
            case LogLevel::error:
                return "error";
            case LogLevel::critical:
                return "critical";
        }
        return "?";
    }

    LogBuffer& LogBuffer::instance() {
        // Same first-use initialisation as the loggers themselves, and for the
        // same reason: subsystems log from their constructors, so anything
        // requiring an explicit setup call would miss the start of the session.
        static LogBuffer buffer;
        return buffer;
    }

    void LogBuffer::push(Record record) {
        const std::lock_guard<std::mutex> lock{mutex};
        records.push_back(std::move(record));
        while (records.size() > capacity) {
            records.pop_front();
        }
        writes++;
    }

    std::vector<LogBuffer::Record> LogBuffer::snapshot() const {
        const std::lock_guard<std::mutex> lock{mutex};
        return std::vector<Record>{records.begin(), records.end()};
    }

    void LogBuffer::clear() {
        const std::lock_guard<std::mutex> lock{mutex};
        records.clear();
        writes++;
    }

    std::uint64_t LogBuffer::revision() const {
        const std::lock_guard<std::mutex> lock{mutex};
        return writes;
    }

}  // namespace ege
