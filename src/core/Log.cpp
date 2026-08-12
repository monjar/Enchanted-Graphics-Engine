#include "core/Log.hpp"

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

        void createLoggers() {
            std::vector<spdlog::sink_ptr> sinks;
            sinks.emplace_back(std::make_shared<spdlog::sinks::stdout_color_sink_mt>());
            // Also to file: a crash usually takes the console with it, and the
            // last few lines before it are the ones worth having.
            sinks.emplace_back(
                std::make_shared<spdlog::sinks::basic_file_sink_mt>("enchanted.log", true));

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
