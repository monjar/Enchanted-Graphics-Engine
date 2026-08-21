#include "script/ScriptModule.hpp"

#include "core/Log.hpp"
#include "script/BehaviorRegistry.hpp"

#include <atomic>
#include <system_error>

#if defined(_WIN32)
#include <windows.h>
#else
#include <dlfcn.h>
#include <unistd.h>
#endif

namespace ege {

    namespace {

        // A name no other load will pick, so reloading does not collide with
        // the copy still mapped from last time. Not a timestamp: two reloads
        // in the same second are exactly what a build-on-save workflow
        // produces.
        std::string uniqueSuffix() {
            static std::atomic<unsigned> counter{0};
            return std::to_string(counter.fetch_add(1, std::memory_order_relaxed));
        }

        std::string lastLoadError() {
#if defined(_WIN32)
            const DWORD code = GetLastError();
            return "error " + std::to_string(code);
#else
            const char* reason = dlerror();
            return reason == nullptr ? "unknown error" : std::string{reason};
#endif
        }

    }  // namespace

    std::string ScriptModule::fileName(const std::string& baseName) {
#if defined(_WIN32)
        return baseName + ".dll";
#elif defined(__APPLE__)
        return "lib" + baseName + ".dylib";
#else
        return "lib" + baseName + ".so";
#endif
    }

    std::unique_ptr<ScriptModule> ScriptModule::load(
        const std::filesystem::path& path, std::string& error) {
        std::error_code errorCode;
        if (!std::filesystem::exists(path, errorCode) || errorCode) {
            error = "no such file: " + path.string();
            return nullptr;
        }

        // Beside the original rather than in a temp directory: a module
        // resolves its own dependencies relative to where it is, and the
        // engine's shared library is next to the original.
        std::filesystem::path copy = path;
        copy += ".loaded" + uniqueSuffix();
        std::filesystem::copy_file(
            path, copy, std::filesystem::copy_options::overwrite_existing, errorCode);
        if (errorCode) {
            error = "cannot copy " + path.string() + ": " + errorCode.message();
            return nullptr;
        }

        // Registrations rather than entries: a reload re-registers names the
        // registry already has, which replaces entries rather than adding
        // them, so counting entries would report every reload as empty.
        const std::size_t before = BehaviorRegistry::instance().registrations();

#if defined(_WIN32)
        void* handle = static_cast<void*>(LoadLibraryA(copy.string().c_str()));
#else
        // RTLD_NOW so a missing symbol is an error here rather than a crash at
        // the first call into it, and RTLD_GLOBAL so the module's own symbols
        // are visible to anything it loads in turn.
        void* handle = dlopen(copy.string().c_str(), RTLD_NOW | RTLD_GLOBAL);
#endif

        if (handle == nullptr) {
            error = lastLoadError();
            std::filesystem::remove(copy, errorCode);
            return nullptr;
        }

#if !defined(_WIN32)
        // The mapping keeps the file alive, so unlinking now leaves nothing
        // behind in the build tree while the module stays loaded for good.
        // Windows holds the file open and this has to wait for the next clean.
        std::filesystem::remove(copy, errorCode);
#endif

        std::unique_ptr<ScriptModule> module{new ScriptModule{}};
        module->handle = handle;
        module->source = path;
        module->loadedCopy = copy;
        module->added = BehaviorRegistry::instance().registrations() - before;

        EGE_INFO(
            "Script module {}: loaded, {} behaviour(s) registered",
            path.filename().string(),
            module->added);
        if (module->added == 0) {
            EGE_WARN(
                "script module {} registered no behaviours; is EGE_BEHAVIOR missing?",
                path.filename().string());
        }
        return module;
    }

    ScriptModule::~ScriptModule() {
        // Deliberately not closed. See the note in the header: the engine
        // holds pointers into this code it cannot enumerate, and outliving
        // them is the only way to be sure none is followed after it is gone.
        handle = nullptr;
    }

}  // namespace ege
