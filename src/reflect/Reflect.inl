// Template definitions for reflect/Reflect.hpp. Included at the bottom of that
// header; not meant to be included directly.

#include "core/Assert.hpp"

namespace ege {

    namespace detail {

        template <typename T>
        std::string nameOf() {
            if constexpr (TypeName<T>::value != nullptr) {
                return TypeName<T>::value;
            } else {
                // Unique but mangled. Anything the editor has to display should
                // declare a name with EGE_TYPE_NAME or EGE_REFLECT.
                return typeid(T).name();
            }
        }

    }  // namespace detail

    template <typename T>
    const T& FieldInfo::valueIn(const void* instance) const {
        EGE_ASSERT(
            fieldType == &TypeRegistry::of<T>(),
            "field '{}' is of type {}, which is not the type requested",
            fieldName,
            fieldType->name());
        return *reinterpret_cast<const T*>(addressIn(instance));
    }

    template <typename T>
    T& FieldInfo::valueIn(void* instance) const {
        EGE_ASSERT(
            fieldType == &TypeRegistry::of<T>(),
            "field '{}' is of type {}, which is not the type requested",
            fieldName,
            fieldType->name());
        return *reinterpret_cast<T*>(addressIn(instance));
    }

    template <typename T>
    template <typename M>
    FieldBuilder TypeBuilder<T>::addField(const char* name, M T::* member) {
        FieldInfo field{};
        field.fieldName = name;
        // Recurses into the field's own type. Safe because the owning TypeInfo
        // is already in the registry before this runs.
        field.fieldType = &TypeRegistry::of<M>();
        field.fieldOffset = detail::memberOffset(member);
        info.typeFields.push_back(field);
        return FieldBuilder{info, info.typeFields.size() - 1};
    }

    template <typename T>
    const TypeInfo& TypeRegistry::of() {
        static_assert(!std::is_reference_v<T>, "cannot reflect a reference type");
        static_assert(!std::is_pointer_v<T>, "cannot reflect a pointer type");

        // Function-local static: thread-safe initialisation, and independent of
        // translation unit ordering.
        static const TypeInfo* info = [] {
            TypeInfo& created =
                instance().add(detail::nameOf<T>(), sizeof(T), alignof(T));
            if constexpr (detail::Describe<T>::reflected) {
                created.reflectedFlag = true;
                TypeBuilder<T> builder{created};
                detail::Describe<T>::apply(builder);
            }
            return &created;
        }();
        return *info;
    }

}  // namespace ege
