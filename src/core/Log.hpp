#pragma once

#include <spdlog/spdlog.h>

#include <memory>

namespace ege {

    // Engine-wide logging.
    //
    // Two loggers rather than one so engine output and application output can be
    // filtered separately - the split every engine wants once a project is being
    // built on top of it.
    //
    // The accessors initialise on first use. That is deliberate rather than
    // lazy: subsystems log from their constructors, and an object's members are
    // constructed before the body of the owner's constructor runs, so any scheme
    // requiring an explicit init() call first is a null dereference waiting to
    // happen. init() remains available to control *when* the cost is paid, but
    // nothing depends on it having been called.
    class Log {
    public:
        // Idempotent and thread-safe. Optional.
        static void init();

        static spdlog::logger& engine();
        static spdlog::logger& client();

        // Both sinks, both loggers. Call before aborting.
        static void flush();

        static bool initialised();
    };

}  // namespace ege

// Engine-internal logging.
#define EGE_TRACE(...) ::ege::Log::engine().trace(__VA_ARGS__)
#define EGE_DEBUG(...) ::ege::Log::engine().debug(__VA_ARGS__)
#define EGE_INFO(...) ::ege::Log::engine().info(__VA_ARGS__)
#define EGE_WARN(...) ::ege::Log::engine().warn(__VA_ARGS__)
#define EGE_ERROR(...) ::ege::Log::engine().error(__VA_ARGS__)
#define EGE_CRITICAL(...) ::ege::Log::engine().critical(__VA_ARGS__)

// Logging for code built on top of the engine.
#define EGE_APP_TRACE(...) ::ege::Log::client().trace(__VA_ARGS__)
#define EGE_APP_DEBUG(...) ::ege::Log::client().debug(__VA_ARGS__)
#define EGE_APP_INFO(...) ::ege::Log::client().info(__VA_ARGS__)
#define EGE_APP_WARN(...) ::ege::Log::client().warn(__VA_ARGS__)
#define EGE_APP_ERROR(...) ::ege::Log::client().error(__VA_ARGS__)
#define EGE_APP_CRITICAL(...) ::ege::Log::client().critical(__VA_ARGS__)
