#include "core/Log.hpp"

#include "core/LogBuffer.hpp"

#include <spdlog/sinks/base_sink.h>
#include <spdlog/sinks/basic_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>

#include <chrono>
#include <mutex>
#include <vector>

namespace ege {

    namespace {

        std::shared_ptr<spdlog::logger> engineLogger;
        std::shared_ptr<spdlog::logger> clientLogger;
        std::once_flag initFlag;

        LogLevel translateLevel(spdlog::level::level_enum level) {
            switch (level) {
                case spdlog::level::trace:
                    return LogLevel::trace;
                case spdlog::level::debug:
                    return LogLevel::debug;
                case spdlog::level::warn:
                    return LogLevel::warn;
                case spdlog::level::err:
                    return LogLevel::error;
                case spdlog::level::critical:
                    return LogLevel::critical;
                default:
                    return LogLevel::info;
            }
        }

        // Keeps the recent log in memory as well as on the terminal, so the
        // editor's console shows the same records rather than a second,
        // separately routed stream that could disagree with the file.
        class MemorySink final : public spdlog::sinks::base_sink<std::mutex> {
        protected:
            void sink_it_(const spdlog::details::log_msg& message) override {
                LogBuffer::Record record{};
                record.level = translateLevel(message.level);
                record.logger.assign(message.logger_name.data(), message.logger_name.size());
                record.message.assign(message.payload.data(), message.payload.size());
                LogBuffer::instance().push(std::move(record));
            }

            void flush_() override {}
        };

        void createLoggers() {
            std::vector<spdlog::sink_ptr> sinks;
            sinks.emplace_back(std::make_shared<spdlog::sinks::stdout_color_sink_mt>());
            // Also to file: a crash usually takes the console with it, and the
            // last few lines before it are the ones worth having.
            sinks.emplace_back(
                std::make_shared<spdlog::sinks::basic_file_sink_mt>("enchanted.log", true));
            sinks.emplace_back(std::make_shared<MemorySink>());

            sinks[0]->set_pattern("%^[%T] %n: %v%$");
            sinks[1]->set_pattern("[%Y-%m-%d %T.%e] [%l] %n: %v");

            engineLogger = std::make_shared<spdlog::logger>("ENGINE", sinks.begin(), sinks.end());
            clientLogger = std::make_shared<spdlog::logger>("APP", sinks.begin(), sinks.end());

            for (const auto& logger : {engineLogger, clientLogger}) {
#ifdef NDEBUG
                logger->set_level(spdlog::level::info);
#else
                logger->set_level(spdlog::level::trace);
#endif
                // Warnings and above are very often the last thing written before
                // something goes wrong, so do not leave them in a buffer.
                logger->flush_on(spdlog::level::warn);
                spdlog::register_logger(logger);
            }

            // flush_on alone only covers warnings and above, so an info-level
            // trail sitting in the buffer is lost if the process is killed
            // rather than unwound - which is exactly the case the file sink
            // exists for. A periodic flush bounds how much can be lost.
            spdlog::flush_every(std::chrono::seconds(2));
        }

    }  // namespace

    void Log::init() {
        std::call_once(initFlag, &createLoggers);
    }

    spdlog::logger& Log::engine() {
        init();
        return *engineLogger;
    }

    spdlog::logger& Log::client() {
        init();
        return *clientLogger;
    }

    void Log::flush() {
        if (engineLogger) {
            engineLogger->flush();
            clientLogger->flush();
        }
    }

    bool Log::initialised() {
        return engineLogger != nullptr;
    }

}  // namespace ege
