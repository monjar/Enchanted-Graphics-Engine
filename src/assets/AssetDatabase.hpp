#pragma once

#include "assets/AssetLoadQueue.hpp"
#include "core/Guid.hpp"
#include "render/Material.hpp"
#include "render/Model.hpp"
#include "rhi/Descriptors.hpp"
#include "rhi/Texture.hpp"

#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace ege {

    class JobSystem;

    enum class AssetKind { unknown, mesh, texture, material, scene };

    const char* assetKindName(AssetKind kind);

    // What the database knows about one asset without having loaded it.
    struct AssetRecord {
        Guid id{};
        AssetKind kind = AssetKind::unknown;
        // Relative to the asset root, so moving a project does not rewrite
        // every sidecar. Empty for assets built in code.
        std::filesystem::path path;
        std::string name;
        // Built in code rather than read from a file: the procedural
        // primitives, the demo's materials, and everything a container asset
        // produces when it is imported.
        bool builtin = false;
    };

    // Maps stable ids to assets.
    //
    // The point of the whole thing is one property: a reference stored in a
    // scene file survives the referenced file being moved, renamed, or
    // reimported. References name an id; the id lives in a `.egameta` sidecar
    // beside the source file; the path is only ever the current answer to
    // "where is it now".
    //
    // Deliberately split so that a machine with no GPU can still exercise the
    // interesting half. Scanning, sidecar creation and id resolution need no
    // device and are unit-tested; loading needs one and is covered by the
    // headless render test instead.
    class AssetDatabase {
    public:
        static AssetDatabase& instance();

        AssetDatabase(const AssetDatabase&) = delete;
        AssetDatabase& operator=(const AssetDatabase&) = delete;

        // Gives the database what it needs to build GPU objects. Before this
        // it can still catalogue and resolve ids, which is all a device-free
        // test wants; loading anything returns null and says so once.
        void attachDevice(
            Device& device, DescriptorPool& materialPool, DescriptorSetLayout& materialLayout);

        // Gives the database somewhere to load asynchronously. Without one,
        // requestAsync loads on the calling thread and reports the result
        // immediately - which is what a test with no engine around it gets,
        // and is the same answer, only sooner.
        void attachJobSystem(JobSystem& jobs);

        bool canLoad() const { return device != nullptr; }

        // Walks `root` and catalogues every importable file, writing a sidecar
        // for any that has none. Returns the number of files catalogued.
        //
        // Sidecars are written, not just read: an id has to exist before the
        // first reference to it does, and asking the user to run an import
        // step before their assets have identities is how a pipeline becomes
        // something people work around.
        std::size_t scan(const std::filesystem::path& root);

        const std::vector<AssetRecord>& all() const { return records; }

        const AssetRecord* find(Guid id) const;

        const AssetRecord* findByPath(const std::filesystem::path& relative) const;

        // Catalogues an asset that exists only in memory. The id is derived
        // from the name, so the same build always produces the same one and a
        // scene referencing it resolves on the next run.
        Guid addMesh(const std::string& name, std::shared_ptr<Model> mesh);
        Guid addMaterial(const std::string& name, std::shared_ptr<Material> material);
        Guid addTexture(const std::string& name, std::shared_ptr<Texture> texture);

        // The same, under an id derived from a container's id rather than a
        // bare name - a mesh inside a glTF file, say. Re-importing the
        // container yields the same ids, which is what lets a scene keep
        // pointing at the third mesh of a model after the model is edited.
        static Guid subAssetId(Guid container, AssetKind kind, std::size_t index);
        Guid addMesh(Guid id, const std::string& name, std::shared_ptr<Model> mesh);
        Guid addMaterial(Guid id, const std::string& name, std::shared_ptr<Material> material);
        Guid addTexture(Guid id, const std::string& name, std::shared_ptr<Texture> texture);

        // Resolve, loading from disk on first use. Null when the id is
        // unknown, the kind is wrong, no device is attached, or loading
        // failed - all four of which the caller handles identically, by
        // drawing nothing.
        std::shared_ptr<Model> mesh(Guid id);
        std::shared_ptr<Material> material(Guid id);
        std::shared_ptr<Texture> texture(Guid id);

        // Starts loading an asset on the job system, if it is not already
        // loaded and not already loading. True when this call started one.
        //
        // Nothing blocks and nothing is returned: the asset appears in the
        // cache when it lands, and the reference that wanted it is re-pointed
        // by whoever calls takeLoaded. A reference that has not resolved draws
        // nothing, which is what it already did before the asset existed.
        bool requestAsync(Guid id);

        // The assets that finished loading since the last call. The caller is
        // expected to re-point whatever was waiting on them.
        std::vector<Guid> takeLoaded();

        std::size_t loadsInFlight() const { return loadQueue.inFlight(); }

        // Reloads an asset from disk after its file changed.
        //
        // A material is rewritten *inside* the object every holder already
        // points at, so a live scene picks up the edit without anything
        // re-resolving. Meshes and textures are immutable once built, so
        // those are dropped from the cache and the next resolve rebuilds
        // them - which is why the caller also refreshes the world's
        // references. Reloading a texture reloads the materials too, since a
        // material's texture slots are the only thing that knows it changed.
        void reload(Guid id);

        // Forgets every record and every loaded asset. For tests, and for an
        // editor opening a different project.
        void clear();

        const std::filesystem::path& root() const { return assetRoot; }

    private:
        AssetDatabase() = default;

        AssetRecord* mutableFind(Guid id);
        void catalogue(AssetRecord record);
        // Reads the id beside `file`, minting and writing one if absent.
        Guid identify(const std::filesystem::path& file, AssetKind kind);

        std::shared_ptr<Material> loadMaterialFile(const AssetRecord& record);

        // Loads on the calling thread, whichever that is, and caches the
        // result. This is what a worker runs.
        void loadNow(Guid id);

        // Cache lookups, guarded. An engaged optional holding null is a load
        // that was tried and failed, which is not the same as one never tried
        // and must not be retried on every reference.
        template<typename T>
        std::optional<std::shared_ptr<T>> cached(
            const std::unordered_map<Guid, std::shared_ptr<T>>& cache, Guid id) const;

        Device* device = nullptr;
        JobSystem* jobs = nullptr;
        DescriptorPool* materialPool = nullptr;
        DescriptorSetLayout* materialLayout = nullptr;
        bool warnedAboutNoDevice = false;

        std::filesystem::path assetRoot;
        std::vector<AssetRecord> records;
        std::unordered_map<Guid, std::size_t> byId;

        // Guards the three caches and the record list against the workers.
        // Held around lookups and insertions only, never around a load: a
        // material's load resolves its textures, and a lock held across that
        // would be a lock taken twice by one thread.
        mutable std::mutex cacheMutex;
        std::unordered_map<Guid, std::shared_ptr<Model>> meshes;
        std::unordered_map<Guid, std::shared_ptr<Material>> materials;
        std::unordered_map<Guid, std::shared_ptr<Texture>> textures;

        AssetLoadQueue loadQueue;
    };

}  // namespace ege
