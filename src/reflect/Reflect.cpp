#include "reflect/Reflect.hpp"

#include <algorithm>

namespace ege {

    const FieldInfo* TypeInfo::field(std::string_view fieldName) const {
        const auto found = std::find_if(
            typeFields.begin(), typeFields.end(), [fieldName](const FieldInfo& candidate) {
                return candidate.name() == fieldName;
            });
        return found == typeFields.end() ? nullptr : &*found;
    }

    FieldInfo& FieldBuilder::field() {
        return type.typeFields[fieldIndex];
    }

    TypeRegistry& TypeRegistry::instance() {
        static TypeRegistry registry;
        return registry;
    }

    TypeInfo& TypeRegistry::add(std::string name, std::size_t size, std::size_t alignment) {
        auto owned = std::make_unique<TypeInfo>();
        owned->typeName = std::move(name);
        owned->typeSize = size;
        owned->typeAlignment = alignment;

        TypeInfo& created = *owned;
        types.push_back(std::move(owned));

        // Keyed on the string owned by the TypeInfo, which never moves because
        // the registry stores pointers rather than values.
        byName.emplace(created.typeName, &created);
        return created;
    }

    const TypeInfo* TypeRegistry::find(std::string_view name) const {
        const auto found = byName.find(name);
        return found == byName.end() ? nullptr : found->second;
    }

    std::vector<const TypeInfo*> TypeRegistry::all() const {
        std::vector<const TypeInfo*> result;
        result.reserve(types.size());
        for (const auto& type : types) {
            result.push_back(type.get());
        }
        return result;
    }

}  // namespace ege
