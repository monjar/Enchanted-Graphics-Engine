#pragma once

#include <filesystem>
#include <memory>
#include <string>

namespace ege {

    // A shared library of behaviours, loaded at runtime.
    //
    // This is what the engine being a shared library was for. A module is
    // built from a project's own sources, links against the engine, and
    // registers its behaviours the way the engine's own do - through
    // EGE_BEHAVIOR, at static-initialisation time, which for a module means
    // the moment it is loaded. Nothing in the engine names the module's types
    // and nothing in the module is listed anywhere central: a scene refers to
    // a behaviour by name, and whether that name resolves is a question of
    // what is loaded.
    //
    // Two decisions worth stating, because both look like sloppiness and are
    // not.
    //
    // **A module is loaded from a copy of itself.** The point of the whole
    // exercise is to rebuild the file while the engine is running, and on
    // Windows a loaded DLL cannot be replaced. On POSIX it can, but replacing
    // it under a running mapping is its own kind of undefined. So the file is
    // copied to a private name and the copy is what gets loaded, which leaves
    // the build free to overwrite the original whenever it likes.
    //
    // **A module is never unloaded.** Not laziness - correctness. Unloading
    // one invalidates every pointer into its code, and the engine holds
    // several kinds without any way to enumerate them: the vtable of every
    // live behaviour instance, the factory std::function the behaviour
    // registry holds, and the field accessors reflection built for its types.
    // Missing any one of them is a crash at an unrelated moment later. A
    // development session that reloads fifty times leaks fifty modules, which
    // is a few megabytes; the alternative is a crash, and the reload is a
    // development-time feature that a shipped game never performs.
    class ScriptModule {
    public:
        // Loads `path`, or returns null and fills `error` with the reason.
        // Behaviours in the module have registered themselves by the time this
        // returns - that is what loading a shared library does.
        static std::unique_ptr<ScriptModule> load(
            const std::filesystem::path& path, std::string& error);

        ~ScriptModule();

        ScriptModule(const ScriptModule&) = delete;
        ScriptModule& operator=(const ScriptModule&) = delete;

        // The file this was built from - what a watcher watches.
        const std::filesystem::path& sourcePath() const { return source; }

        // How many behaviours the registry held before and after loading. The
        // difference is what this module contributed, and a difference of zero
        // means a module that loaded and registered nothing - which is a
        // build that did not include what the developer thought it did.
        std::size_t behaviorsAdded() const { return added; }

        // The conventional file name for a module of this base name on this
        // platform: libFoo.so, Foo.dll, libFoo.dylib.
        static std::string fileName(const std::string& baseName);

    private:
        ScriptModule() = default;

        void* handle = nullptr;
        std::filesystem::path source;
        std::filesystem::path loadedCopy;
        std::size_t added = 0;
    };

}  // namespace ege
