#include "assets/AssetDatabase.hpp"

#include "core/Log.hpp"
#include "reflect/Serialization.hpp"

#include <algorithm>
#include <fstream>
#include <nlohmann/json.hpp>
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
        meshes[id] = std::move(asset);
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
        materials[id] = std::move(asset);
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
        textures[id] = std::move(asset);
        return id;
    }

    std::shared_ptr<Model> AssetDatabase::mesh(Guid id) {
        const auto cached = meshes.find(id);
        if (cached != meshes.end()) {
            return cached->second;
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
            meshes[id] = loaded;
            return loaded;
        } catch (const std::exception& error) {
            EGE_ERROR("failed to load mesh {}: {}", record->path.string(), error.what());
            // Cached as null so a broken asset is reported once rather than
            // retried every time something references it.
            meshes[id] = nullptr;
            return nullptr;
        }
    }

    std::shared_ptr<Texture> AssetDatabase::texture(Guid id) {
        const auto cached = textures.find(id);
        if (cached != textures.end()) {
            return cached->second;
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
            textures[id] = loaded;
            return loaded;
        } catch (const std::exception& error) {
            EGE_ERROR("failed to load texture {}: {}", record->path.string(), error.what());
            textures[id] = nullptr;
            return nullptr;
        }
    }

    std::shared_ptr<Material> AssetDatabase::material(Guid id) {
        const auto cached = materials.find(id);
        if (cached != materials.end()) {
            return cached->second;
        }

        const AssetRecord* record = find(id);
        if (record == nullptr || record->kind != AssetKind::material || record->path.empty()) {
            return nullptr;
        }
        if (!canLoad()) {
            return nullptr;
        }

        try {
            std::shared_ptr<Material> loaded = loadMaterialFile(*record);
            materials[id] = loaded;
            return loaded;
        } catch (const std::exception& error) {
            EGE_ERROR("failed to load material {}: {}", record->path.string(), error.what());
            materials[id] = nullptr;
            return nullptr;
        }
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

        auto loaded = std::make_shared<Material>(*device, *materialPool, *materialLayout);

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
                const auto cached = materials.find(id);
                if (cached == materials.end() || cached->second == nullptr) {
                    // Never loaded, or last load failed: dropping the entry is
                    // enough, and the next resolve tries again.
                    materials.erase(id);
                    return;
                }
                try {
                    // Read into a scratch material, then move the parts across
                    // into the live one. Reading straight into it would leave
                    // a half-updated material visible if the file is broken.
                    const std::shared_ptr<Material> fresh = loadMaterialFile(*record);
                    cached->second->adoptFrom(*fresh);
                    EGE_INFO("Reloaded material {}", record->path.string());
                } catch (const std::exception& error) {
                    EGE_ERROR("reloading {} failed: {}", record->path.string(), error.what());
                }
                return;
            }
            case AssetKind::texture:
                textures.erase(id);
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
            case AssetKind::mesh:
                meshes.erase(id);
                EGE_INFO("Reloaded mesh {}", record->path.string());
                return;
            case AssetKind::scene:
            case AssetKind::unknown:
                return;
        }
    }

    void AssetDatabase::clear() {
        records.clear();
        byId.clear();
        meshes.clear();
        materials.clear();
        textures.clear();
        assetRoot.clear();
    }

}  // namespace ege
