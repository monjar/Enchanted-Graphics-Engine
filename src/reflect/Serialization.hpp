#pragma once

#include "reflect/Reflect.hpp"

#include <functional>
#include <nlohmann/json.hpp>
#include <string>
#include <unordered_map>

namespace ege {

    // Reads and writes reflected values as JSON.
    //
    // Composite types are handled generically by walking their fields, so a
    // struct becomes serializable the moment it is reflected and nothing has to
    // be written per type. Leaves cannot be - a float and a vec3 are both
    // opaque bytes to reflection - so each leaf type registers a pair of
    // conversion functions once, here.
    //
    // That split is the whole design: one converter per primitive, and every
    // composite for free.
    class Serializer {
    public:
        using Writer = std::function<nlohmann::json(const void*)>;
        using Reader = std::function<void(void*, const nlohmann::json&)>;

        static Serializer& instance();

        // Registers conversions for a leaf type. Later registrations replace
        // earlier ones, so a project can override how a type is stored.
        template<typename T>
        void registerLeaf(Writer writer, Reader reader);

        // Serialises an instance of `type` at `instance`. Recurses through
        // reflected fields; leaves go through their registered writer.
        nlohmann::json write(const TypeInfo& type, const void* instance) const;

        // Applies `json` onto an existing instance. Fields absent from the
        // JSON keep whatever value the instance already had, so a scene saved
        // by an older build still loads and simply leaves new fields at their
        // defaults.
        void read(const TypeInfo& type, void* instance, const nlohmann::json& json) const;

        bool hasLeafConverter(const TypeInfo& type) const;

    private:
        struct Converter {
            Writer writer;
            Reader reader;
        };

        std::unordered_map<const TypeInfo*, Converter> converters;
    };

    // Registers the built-in leaf conversions: the integer and floating point
    // types, bool, string, and the glm vector and matrix types.
    void registerBuiltinSerializers();

}  // namespace ege

#include "reflect/Serialization.inl"
