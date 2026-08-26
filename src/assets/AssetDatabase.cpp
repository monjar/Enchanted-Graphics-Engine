#include "assets/AssetDatabase.hpp"

#include "core/JobSystem.hpp"
#include "core/Log.hpp"
#include "reflect/Serialization.hpp"
#include "scene/Prefab.hpp"

#include <algorithm>
#include <fstream>
#include <nlohmann/json.hpp>
#include <sstream>
#include <stdexcept>

namespace ege {

    namespace {

        constexpr const char* sidecarExtension = ".egameta";
        constexpr int sidecarVersion = 1;
        constexpr int materialFormatVersion = 1;

        AssetKind kindForExtension(const std::filesystem::path& path) {
            std::string extension = path.extension().string();
            std::transform(
                extension.begin(), extension.end(), extension.begin(), [](unsigned char character) {
                    return static_cast<char>(std::tolower(character));
                });

            if (extension == ".gltf" || extension == ".glb") {
                return AssetKind::scene;
            }
            if (extension == ".obj") {
                return AssetKind::mesh;
            }
            if (extension == ".png" || extension == ".jpg" || extension == ".jpeg" ||
                extension == ".tga" || extension == ".bmp" || extension == ".hdr") {
                return AssetKind::texture;
            }
            if (extension == ".egeprefab") {
                return AssetKind::prefab;
            }
            if (extension == ".egematerial") {
                return AssetKind::material;
            }
            return AssetKind::unknown;
        }

        Guid readSidecar(const std::filesystem::path& sidecar) {
            std::ifstream file{sidecar};
            if (!file) {
                return Guid{};
            }
            try {
                const nlohmann::json json = nlohmann::json::parse(file);
                const std::optional<Guid> id = Guid::parse(json.value("guid", std::string{}));
                return id.value_or(Guid{});
            } catch (const std::exception& error) {
                // A corrupt sidecar is worth saying out loud: it is about to
                // be replaced, and every reference to the old id will dangle.
                EGE_WARN("unreadable asset sidecar {}: {}", sidecar.string(), error.what());
                return Guid{};
            }
        }

        void writeSidecar(const std::filesystem::path& sidecar, Guid id, AssetKind kind) {
            nlohmann::json json = nlohmann::json::object();
            json["version"] = sidecarVersion;
            json["guid"] = id.toString();
            json["kind"] = assetKindName(kind);

            std::ofstream file{sidecar};
            if (!file) {
                EGE_WARN("cannot write asset sidecar {}", sidecar.string());
                return;
            }
            file << json.dump(2) << '\n';
        }

    }  // namespace

    const char* assetKindName(AssetKind kind) {
        switch (kind) {
            case AssetKind::mesh:
                return "mesh";
            case AssetKind::texture:
                return "texture";
            case AssetKind::material:
                return "material";
            case AssetKind::scene:
                return "scene";
            case AssetKind::prefab:
                return "prefab";
            case AssetKind::unknown:
                break;
        }
        return "unknown";
    }

    AssetDatabase& AssetDatabase::instance() {
        static AssetDatabase database;
        return database;
    }

    void AssetDatabase::attachDevice(
        Device& deviceRef, DescriptorPool& poolRef, DescriptorSetLayout& layoutRef) {
        device = &deviceRef;
        materialPool = &poolRef;
        materialLayout = &layoutRef;
    }

    Guid AssetDatabase::identify(const std::filesystem::path& file, AssetKind kind) {
        std::filesystem::path sidecar = file;
        sidecar += sidecarExtension;

        const Guid existing = readSidecar(sidecar);
        if (!existing.isNull()) {
            return existing;
        }

        const Guid minted = Guid::generate();
        writeSidecar(sidecar, minted, kind);
        EGE_DEBUG("asset {} identified as {}", file.filename().string(), minted.toString());
        return minted;
    }

    std::size_t AssetDatabase::scan(const std::filesystem::path& root) {
        assetRoot = root;

        std::error_code errorCode;
        if (!std::filesystem::is_directory(root, errorCode)) {
            EGE_WARN("asset root {} does not exist; nothing catalogued", root.string());
            return 0;
        }

        std::size_t catalogued = 0;
        for (const std::filesystem::directory_entry& entry :
             std::filesystem::recursive_directory_iterator{root, errorCode}) {
            if (!entry.is_regular_file(errorCode)) {
                continue;
            }
            const std::filesystem::path& path = entry.path();
            if (path.extension() == sidecarExtension) {
                continue;
            }

            const AssetKind kind = kindForExtension(path);
            if (kind == AssetKind::unknown) {
                continue;
            }

            AssetRecord record{};
            record.id = identify(path, kind);
            record.kind = kind;
            record.path = std::filesystem::relative(path, root, errorCode);
            record.name = path.stem().string();
            catalogue(std::move(record));
            catalogued++;
        }

        EGE_INFO("Asset database: {} files catalogued under {}", catalogued, root.string());
        return catalogued;
    }

    void AssetDatabase::catalogue(AssetRecord record) {
        const auto existing = byId.find(record.id);
        if (existing != byId.end()) {
            // Two files claiming one id means a sidecar was copied along with
            // its asset. Keeping the first is arbitrary; saying so is not.
            if (records[existing->second].path != record.path) {
                EGE_WARN(
                    "asset id {} claimed by both {} and {}; keeping the first",
                    record.id.toString(),
                    records[existing->second].path.string(),
                    record.path.string());
            }
            records[existing->second] = std::move(record);
            return;
        }

        byId.emplace(record.id, records.size());
        records.push_back(std::move(record));
    }

    const AssetRecord* AssetDatabase::find(Guid id) const {
        const auto found = byId.find(id);
        return found == byId.end() ? nullptr : &records[found->second];
    }

    AssetRecord* AssetDatabase::mutableFind(Guid id) {
        const auto found = byId.find(id);
        return found == byId.end() ? nullptr : &records[found->second];
    }

    const AssetRecord* AssetDatabase::findByPath(const std::filesystem::path& relative) const {
        const auto found =
            std::find_if(records.begin(), records.end(), [&relative](const AssetRecord& record) {
                return record.path == relative;
            });
        return found == records.end() ? nullptr : &*found;
    }

    Guid AssetDatabase::subAssetId(Guid container, AssetKind kind, std::size_t index) {
        return Guid::fromName(
            container.toString() + "/" + assetKindName(kind) + "/" + std::to_string(index));
    }

    Guid AssetDatabase::addMesh(const std::string& name, std::shared_ptr<Model> asset) {
        return addMesh(Guid::fromName("mesh:" + name), name, std::move(asset));
    }

    Guid AssetDatabase::addMaterial(const std::string& name, std::shared_ptr<Material> asset) {
        return addMaterial(Guid::fromName("material:" + name), name, std::move(asset));
    }

    Guid AssetDatabase::addTexture(const std::string& name, std::shared_ptr<Texture> asset) {
        return addTexture(Guid::fromName("texture:" + name), name, std::move(asset));
    }

    Guid AssetDatabase::addMesh(Guid id, const std::string& name, std::shared_ptr<Model> asset) {
        AssetRecord record{};
        record.id = id;
        record.kind = AssetKind::mesh;
        record.name = name;
        record.builtin = true;
        catalogue(std::move(record));
        {
            const std::lock_guard<std::mutex> lock{cacheMutex};
            meshes[id] = std::move(asset);
        }
        return id;
    }

    Guid AssetDatabase::addMaterial(
        Guid id, const std::string& name, std::shared_ptr<Material> asset) {
        AssetRecord record{};
        record.id = id;
        record.kind = AssetKind::material;
        record.name = name;
        record.builtin = true;
        catalogue(std::move(record));
        {
            const std::lock_guard<std::mutex> lock{cacheMutex};
            materials[id] = std::move(asset);
        }
        return id;
    }

    Guid AssetDatabase::addTexture(
        Guid id, const std::string& name, std::shared_ptr<Texture> asset) {
        AssetRecord record{};
        record.id = id;
        record.kind = AssetKind::texture;
        record.name = name;
        record.builtin = true;
        catalogue(std::move(record));
        {
            const std::lock_guard<std::mutex> lock{cacheMutex};
            textures[id] = std::move(asset);
        }
        return id;
    }

    template<typename T>
    std::optional<std::shared_ptr<T>> AssetDatabase::cached(
        const std::unordered_map<Guid, std::shared_ptr<T>>& cache, Guid id) const {
        const std::lock_guard<std::mutex> lock{cacheMutex};
        const auto found = cache.find(id);
        if (found == cache.end()) {
            return std::nullopt;
        }
        return found->second;
    }

    std::shared_ptr<Model> AssetDatabase::mesh(Guid id) {
        if (const auto hit = cached(meshes, id)) {
            return *hit;
        }

        const AssetRecord* record = find(id);
        if (record == nullptr || record->kind != AssetKind::mesh || record->path.empty()) {
            return nullptr;
        }
        if (!canLoad()) {
            if (!warnedAboutNoDevice) {
                warnedAboutNoDevice = true;
                EGE_WARN("asset database has no device; nothing can be loaded");
            }
            return nullptr;
        }

        try {
            std::shared_ptr<Model> loaded =
                Model::createModelFromFile(*device, (assetRoot / record->path).string());
            const std::lock_guard<std::mutex> lock{cacheMutex};
            meshes[id] = loaded;
            return loaded;
        } catch (const std::exception& error) {
            EGE_ERROR("failed to load mesh {}: {}", record->path.string(), error.what());
            // Cached as null so a broken asset is reported once rather than
            // retried every time something references it.
            const std::lock_guard<std::mutex> lock{cacheMutex};
            meshes[id] = nullptr;
            return nullptr;
        }
    }

    std::shared_ptr<Texture> AssetDatabase::texture(Guid id) {
        if (const auto hit = cached(textures, id)) {
            return *hit;
        }

        const AssetRecord* record = find(id);
        if (record == nullptr || record->kind != AssetKind::texture || record->path.empty()) {
            return nullptr;
        }
        if (!canLoad()) {
            return nullptr;
        }

        try {
            std::shared_ptr<Texture> loaded =
                Texture::fromFile(*device, (assetRoot / record->path).string());
            const std::lock_guard<std::mutex> lock{cacheMutex};
            textures[id] = loaded;
            return loaded;
        } catch (const std::exception& error) {
            EGE_ERROR("failed to load texture {}: {}", record->path.string(), error.what());
            const std::lock_guard<std::mutex> lock{cacheMutex};
            textures[id] = nullptr;
            return nullptr;
        }
    }

    std::shared_ptr<Material> AssetDatabase::material(Guid id) {
        if (const auto hit = cached(materials, id)) {
            return *hit;
        }

        const AssetRecord* record = find(id);
        if (record == nullptr || record->kind != AssetKind::material || record->path.empty()) {
            return nullptr;
        }
        if (!canLoad()) {
            return nullptr;
        }

        try {
            // Outside the lock: this resolves the material's textures, and a
            // lock held across that would be one this thread already holds.
            std::shared_ptr<Material> loaded = loadMaterialFile(*record);
            const std::lock_guard<std::mutex> lock{cacheMutex};
            materials[id] = loaded;
            return loaded;
        } catch (const std::exception& error) {
            EGE_ERROR("failed to load material {}: {}", record->path.string(), error.what());
            const std::lock_guard<std::mutex> lock{cacheMutex};
            materials[id] = nullptr;
            return nullptr;
        }
    }

    std::shared_ptr<Prefab> AssetDatabase::prefab(Guid id) {
        if (const auto hit = cached(prefabs, id)) {
            return *hit;
        }

        const AssetRecord* record = find(id);
        if (record == nullptr || record->kind != AssetKind::prefab || record->path.empty()) {
            return nullptr;
        }
        // No canLoad() check, deliberately: a prefab is text, and the
        // machinery that turns it into entities is the scene serializer's
        // rather than the GPU's.

        std::ifstream file{assetRoot / record->path};
        if (!file) {
            EGE_ERROR("cannot open prefab {}", record->path.string());
            const std::lock_guard<std::mutex> lock{cacheMutex};
            prefabs[id] = nullptr;
            return nullptr;
        }
        // Through the stream buffer rather than an istreambuf_iterator pair:
        // the iterator form makes GCC's null-dereference analysis give up
        // inside libstdc++ and warn, and this reads the same.
        std::ostringstream contents;
        contents << file.rdbuf();
        auto loaded = std::make_shared<Prefab>();
        loaded->document = contents.str();

        const std::lock_guard<std::mutex> lock{cacheMutex};
        prefabs[id] = loaded;
        return loaded;
    }

    std::shared_ptr<Material> AssetDatabase::loadMaterialFile(const AssetRecord& record) {
        std::ifstream file{assetRoot / record.path};
        if (!file) {
            throw std::runtime_error{"cannot open " + record.path.string()};
        }
        const nlohmann::json json = nlohmann::json::parse(file);

        const int version = json.value("version", 0);
        if (version != materialFormatVersion) {
            EGE_WARN(
                "material {} written in format version {}, loader expects {}",
                record.path.string(),
                version,
                materialFormatVersion);
        }

        auto loaded = std::make_shared<Material>(*materialPool, *materialLayout);

        // Properties go through the same reflection-driven serializer the
        // scene uses, so a field added to MaterialProperties is readable here
        // without touching this function.
        const auto properties = json.find("properties");
        if (properties != json.end()) {
            Serializer::instance().read(
                TypeRegistry::of<MaterialProperties>(), &loaded->properties, *properties);
        }

        // Texture slots are ids, not paths, for the same reason everything
        // else is.
        const auto slot = [&](const char* key) -> std::shared_ptr<Texture> {
            const std::optional<Guid> id = Guid::parse(json.value(key, std::string{}));
            return id.has_value() ? texture(*id) : nullptr;
        };
        loaded->setBaseColor(slot("baseColor"));
        loaded->setNormalMap(slot("normal"));
        loaded->setMetallicRoughness(slot("metallicRoughness"));
        loaded->setEmissive(slot("emissive"));
        loaded->updateDescriptorSet();
        return loaded;
    }

    void AssetDatabase::reload(Guid id) {
        const AssetRecord* record = find(id);
        if (record == nullptr || record->path.empty() || !canLoad()) {
            return;
        }

        switch (record->kind) {
            case AssetKind::material: {
                const std::shared_ptr<Material> live = [&]() {
                    const std::lock_guard<std::mutex> lock{cacheMutex};
                    const auto found = materials.find(id);
                    if (found == materials.end() || found->second == nullptr) {
                        // Never loaded, or the last load failed: dropping the
                        // entry is enough, and the next resolve tries again.
                        materials.erase(id);
                        return std::shared_ptr<Material>{};
                    }
                    return found->second;
                }();
                if (live == nullptr) {
                    return;
                }
                try {
                    // Read into a scratch material, then move the parts across
                    // into the live one. Reading straight into it would leave
                    // a half-updated material visible if the file is broken.
                    const std::shared_ptr<Material> fresh = loadMaterialFile(*record);
                    live->adoptFrom(*fresh);
                    EGE_INFO("Reloaded material {}", record->path.string());
                } catch (const std::exception& error) {
                    EGE_ERROR("reloading {} failed: {}", record->path.string(), error.what());
                }
                return;
            }
            case AssetKind::texture: {
                const std::lock_guard<std::mutex> lock{cacheMutex};
                textures.erase(id);
            }
                // Nothing holds a texture except the materials that sample it,
                // and a material only learns of the new one by being read
                // again. There are few of them and they are cheap.
                for (const AssetRecord& other : records) {
                    if (other.kind == AssetKind::material) {
                        reload(other.id);
                    }
                }
                EGE_INFO("Reloaded texture {}", record->path.string());
                return;
            case AssetKind::mesh: {
                const std::lock_guard<std::mutex> lock{cacheMutex};
                meshes.erase(id);
            }
                EGE_INFO("Reloaded mesh {}", record->path.string());
                return;
            case AssetKind::prefab: {
                // Dropping the document is the whole reload: nothing holds a
                // prefab open, and what was already spawned from it is
                // entities now rather than a copy of the file.
                const std::lock_guard<std::mutex> lock{cacheMutex};
                prefabs.erase(id);
            }
                EGE_INFO("Reloaded prefab {}", record->path.string());
                return;
            case AssetKind::scene:
            case AssetKind::unknown:
                return;
        }
    }

    void AssetDatabase::attachJobSystem(JobSystem& system) {
        jobs = &system;
    }

    bool AssetDatabase::requestAsync(Guid id) {
        const AssetRecord* record = find(id);
        if (record == nullptr || record->path.empty() || !canLoad()) {
            return false;
        }

        // Already there, or already known to be unloadable. Either way this
        // call has nothing to start.
        switch (record->kind) {
            case AssetKind::mesh:
                if (cached(meshes, id).has_value()) {
                    return false;
                }
                break;
            case AssetKind::texture:
                if (cached(textures, id).has_value()) {
                    return false;
                }
                break;
            case AssetKind::material:
                if (cached(materials, id).has_value()) {
                    return false;
                }
                break;
            case AssetKind::prefab:
            case AssetKind::scene:
            case AssetKind::unknown:
                // A prefab is a text file that no frame is waiting on, so
                // there is nothing worth queueing: it loads when it is asked
                // for.
                return false;
        }

        // Somebody else asked first. Their load is this caller's load.
        if (!loadQueue.request(id)) {
            return false;
        }

        if (jobs == nullptr) {
            // No pool to hand it to, so it happens here. The caller learns of
            // it through takeLoaded either way, only sooner.
            loadNow(id);
            loadQueue.finish(id);
            return true;
        }

        jobs->submit([this, id]() {
            loadNow(id);
            loadQueue.finish(id);
        });
        return true;
    }

    void AssetDatabase::loadNow(Guid id) {
        const AssetRecord* record = find(id);
        if (record == nullptr) {
            return;
        }
        switch (record->kind) {
            case AssetKind::mesh:
                mesh(id);
                return;
            case AssetKind::texture:
                texture(id);
                return;
            case AssetKind::material:
                material(id);
                return;
            case AssetKind::prefab:
                prefab(id);
                return;
            case AssetKind::scene:
            case AssetKind::unknown:
                return;
        }
    }

    std::vector<Guid> AssetDatabase::takeLoaded() {
        return loadQueue.takeCompleted();
    }

    void AssetDatabase::clear() {
        // Nothing may be mid-load while the caches go away. Waiting on the
        // whole pool is heavier than waiting on these loads alone, and this
        // happens when a project closes rather than during a frame.
        if (jobs != nullptr) {
            jobs->waitForAll();
        }
        loadQueue.clear();
        jobs = nullptr;

        const std::lock_guard<std::mutex> lock{cacheMutex};
        records.clear();
        byId.clear();
        meshes.clear();
        materials.clear();
        textures.clear();
        assetRoot.clear();
    }

}  // namespace ege
