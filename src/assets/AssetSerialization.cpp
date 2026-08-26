#include "assets/AssetSerialization.hpp"

#include "assets/AssetDatabase.hpp"
#include "assets/AssetRef.hpp"
#include "reflect/Serialization.hpp"
#include "scene/Prefab.hpp"

#include <optional>
#include <string>

namespace ege {

    namespace {

        // Only the id is written. The pointer beside it is this process's
        // answer to that id and would be meaningless in the next one, which is
        // the distinction that makes a reference storable at all.
        template<typename T, typename Resolve>
        void registerRef(Serializer& serializer, Resolve resolve) {
            serializer.registerLeaf<AssetRef<T>>(
                [](const void* instance) {
                    const auto* ref = static_cast<const AssetRef<T>*>(instance);
                    // Parentheses, not braces: nlohmann reads a braced string
                    // as a one-element array.
                    return nlohmann::json(ref->id.isNull() ? std::string{} : ref->id.toString());
                },
                [resolve](void* instance, const nlohmann::json& json) {
                    auto* ref = static_cast<AssetRef<T>*>(instance);
                    *ref = AssetRef<T>{};
                    if (!json.is_string()) {
                        return;
                    }
                    const std::string text = json.get<std::string>();
                    if (text.empty()) {
                        return;
                    }
                    const std::optional<Guid> id = Guid::parse(text);
                    if (!id.has_value()) {
                        // Deliberately not silent and deliberately not fatal:
                        // the entity loads without geometry, and the log says
                        // which id could not be read.
                        EGE_WARN("scene holds a malformed asset id '{}'", text);
                        return;
                    }
                    ref->id = *id;
                    // Null when the asset is missing from this project, or when
                    // there is no device to load it with. Both leave a
                    // reference that still knows what it wants, so saving the
                    // scene again does not quietly drop it.
                    ref->value = resolve(*id);
                });
        }

    }  // namespace

    void registerAssetSerializers() {
        Serializer& serializer = Serializer::instance();
        AssetDatabase& database = AssetDatabase::instance();

        registerRef<Model>(serializer, [&database](Guid id) { return database.mesh(id); });
        registerRef<Material>(serializer, [&database](Guid id) { return database.material(id); });
        registerRef<Prefab>(serializer, [&database](Guid id) { return database.prefab(id); });
    }

}  // namespace ege
