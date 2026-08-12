#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <type_traits>
#include <typeinfo>
#include <unordered_map>
#include <vector>

// Runtime reflection.
//
// A type opts in by declaring its fields:
//
//     EGE_REFLECT(TransformComponent)
//         EGE_FIELD(translation);
//         EGE_FIELD(rotation).range(-180.f, 180.f).tooltip("degrees");
//         EGE_FIELD(scale);
//     EGE_REFLECT_END()
//
// and is then describable at runtime: field names, types, offsets, and generic
// read/write through a void*. This is the single mechanism three later
// subsystems are built on - the editor inspector draws from it, scene
// serialization reads and writes through it, and script hot-reload uses it to
// preserve behaviour state across a module reload. It is worth getting right
// once rather than growing three parallel half-solutions.
//
// Reflection is registered lazily on first use of TypeRegistry::of<T>(), and
// eagerly for types listed in Reflection.cpp so that lookup by name - which
// deserialization and the editor's type pickers need - works without the type
// having been touched first.

namespace ege {

    // ---- Field attributes -------------------------------------------------

    // Clamps the editor's control. Does not constrain the value in code.
    struct Range {
        float min = 0.f;
        float max = 0.f;
    };

    // Hover text in the inspector.
    struct Tooltip {
        const char* text = nullptr;
    };

    // Draw as a colour picker rather than as raw components.
    struct AsColor {};

    // Visible in the inspector but not editable.
    struct ReadOnly {};

    class TypeInfo;

    template<typename T>
    class TypeBuilder;

    // Human-readable name for a type. EGE_REFLECT specialises this; leaves that
    // the inspector needs to draw by name declare it with EGE_TYPE_NAME. Types
    // that do neither fall back to the compiler's mangled name, which is unique
    // but not presentable.
    template<typename T>
    struct TypeName {
        static constexpr const char* value = nullptr;
    };

    // ---- Fields -----------------------------------------------------------

    class FieldInfo {
    public:
        std::string_view name() const { return fieldName; }

        const TypeInfo& type() const { return *fieldType; }

        std::size_t offset() const { return fieldOffset; }

        bool hasRange() const { return rangeSet; }

        Range range() const { return fieldRange; }

        const char* tooltip() const { return fieldTooltip; }

        bool isColor() const { return colorFlag; }

        bool isReadOnly() const { return readOnlyFlag; }

        // Address of this field within an instance. The caller is responsible
        // for the instance actually being of the owning type; there is no way
        // to check that through a void*.
        void* addressIn(void* instance) const {
            return static_cast<std::byte*>(instance) + fieldOffset;
        }

        const void* addressIn(const void* instance) const {
            return static_cast<const std::byte*>(instance) + fieldOffset;
        }

        // Typed access. Asserts in debug that T matches the declared type.
        template<typename T>
        T& valueIn(void* instance) const;

        template<typename T>
        const T& valueIn(const void* instance) const;

    private:
        template<typename T>
        friend class TypeBuilder;
        friend class FieldBuilder;

        const char* fieldName = nullptr;
        const TypeInfo* fieldType = nullptr;
        std::size_t fieldOffset = 0;

        Range fieldRange{};
        bool rangeSet = false;
        const char* fieldTooltip = nullptr;
        bool colorFlag = false;
        bool readOnlyFlag = false;
    };

    // ---- Types ------------------------------------------------------------

    class TypeInfo {
    public:
        std::string_view name() const { return typeName; }

        std::size_t size() const { return typeSize; }

        std::size_t alignment() const { return typeAlignment; }

        const std::vector<FieldInfo>& fields() const { return typeFields; }

        // Null when no field of that name exists.
        const FieldInfo* field(std::string_view fieldName) const;

        // True for types with no reflected fields - the leaves the inspector
        // has to draw directly, such as float, bool and glm::vec3.
        bool isLeaf() const { return typeFields.empty(); }

    private:
        template<typename T>
        friend class TypeBuilder;
        friend class FieldBuilder;
        friend class TypeRegistry;

        std::string typeName;
        std::size_t typeSize = 0;
        std::size_t typeAlignment = 0;
        std::vector<FieldInfo> typeFields;
    };

    // ---- Describing a type ------------------------------------------------

    namespace detail {

        // Offset of a member, computed against real storage rather than a null
        // pointer so that UBSan does not object.
        template<typename C, typename M>
        std::size_t memberOffset(M C::*member) {
            alignas(C) static std::byte storage[sizeof(C)];
            const auto* base = reinterpret_cast<const std::byte*>(&storage);
            const auto* field = reinterpret_cast<const std::byte*>(
                &(reinterpret_cast<const C*>(&storage)->*member));
            return static_cast<std::size_t>(field - base);
        }

        // Specialised by EGE_REFLECT. The primary template describes nothing,
        // which is what makes an unreflected type a leaf.
        template<typename T>
        struct Describe {
            static constexpr bool reflected = false;

            static void apply(TypeBuilder<T>&) {}
        };

    }  // namespace detail

    // Returned by EGE_FIELD so attributes chain onto the field they describe:
    //
    //     EGE_FIELD(rotation).range(-180.f, 180.f).tooltip("degrees");
    //
    // Chaining rather than trailing macro arguments keeps EGE_FIELD
    // non-variadic, which matters because a variadic macro invoked with no
    // variadic argument is ill-formed before C++20 and -Wpedantic rejects it.
    //
    // Holds an index rather than a reference because the field vector
    // reallocates as later fields are added.
    class FieldBuilder {
    public:
        FieldBuilder(TypeInfo& owner, std::size_t index) : type{owner}, fieldIndex{index} {}

        FieldBuilder& range(float min, float max) {
            field().fieldRange = Range{min, max};
            field().rangeSet = true;
            return *this;
        }

        FieldBuilder& tooltip(const char* text) {
            field().fieldTooltip = text;
            return *this;
        }

        FieldBuilder& asColor() {
            field().colorFlag = true;
            return *this;
        }

        FieldBuilder& readOnly() {
            field().readOnlyFlag = true;
            return *this;
        }

    private:
        FieldInfo& field();

        TypeInfo& type;
        std::size_t fieldIndex;
    };

    template<typename T>
    class TypeBuilder {
    public:
        explicit TypeBuilder(TypeInfo& target) : info{target} {}

        template<typename M>
        FieldBuilder addField(const char* name, M T::*member);

    private:
        TypeInfo& info;
    };

    // ---- Registry ---------------------------------------------------------

    class TypeRegistry {
    public:
        static TypeRegistry& instance();

        // Describes T, registering it on first call. Stable for the process
        // lifetime, so the returned pointer can be stored.
        template<typename T>
        static const TypeInfo& of();

        // Null when nothing of that name has been registered. Only finds types
        // that have been instantiated through of<T>() or listed for eager
        // registration in Reflection.cpp.
        const TypeInfo* find(std::string_view name) const;

        std::vector<const TypeInfo*> all() const;

    private:
        template<typename T>
        friend const TypeInfo& registerType();

        TypeInfo& add(std::string name, std::size_t size, std::size_t alignment);

        // Stable addresses: the registry hands out references that outlive
        // further insertions, so the storage must not reallocate its elements.
        std::vector<std::unique_ptr<TypeInfo>> types;
        std::unordered_map<std::string_view, TypeInfo*> byName;
    };

    // Forces T into the registry. Reflection.cpp calls this for the built-in
    // types so that lookup by name works before anything has touched them.
    template<typename T>
    const TypeInfo& registerType() {
        return TypeRegistry::of<T>();
    }

}  // namespace ege

// ---- Macros ---------------------------------------------------------------

#define EGE_TYPE_NAME(Type, Name)                      \
    namespace ege {                                    \
        template<>                                     \
        struct TypeName<Type> {                        \
            static constexpr const char* value = Name; \
        };                                             \
    }

#define EGE_REFLECT(Type)                           \
    EGE_TYPE_NAME(Type, #Type)                      \
    namespace ege::detail {                         \
        template<>                                  \
        struct Describe<Type> {                     \
            using Self = Type;                      \
            static constexpr bool reflected = true; \
            static void apply(::ege::TypeBuilder<Type>& builder) {
#define EGE_FIELD(FieldName) builder.addField(#FieldName, &Self::FieldName)

#define EGE_REFLECT_END() \
    }                     \
    }                     \
    ;                     \
    }

#include "reflect/Reflect.inl"
