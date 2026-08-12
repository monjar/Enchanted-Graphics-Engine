#include "reflect/Serialization.hpp"

#include "core/Log.hpp"
#include "reflect/BuiltinTypes.hpp"

#include <glm/glm.hpp>

namespace ege {

    namespace {

        // Vectors and matrices are stored as flat arrays of floats rather than
        // objects with named components. Shorter, and it round-trips exactly.
        template<typename VecT, int N>
        Serializer::Writer vectorWriter() {
            return [](const void* instance) {
                const auto& value = *static_cast<const VecT*>(instance);
                nlohmann::json array = nlohmann::json::array();
                for (int i = 0; i < N; i++) {
                    array.push_back(value[i]);
                }
                return array;
            };
        }

        template<typename VecT, int N>
        Serializer::Reader vectorReader() {
            return [](void* instance, const nlohmann::json& json) {
                if (!json.is_array() || json.size() != N) {
                    return;
                }
                auto& value = *static_cast<VecT*>(instance);
                for (int i = 0; i < N; i++) {
                    value[i] = json[static_cast<std::size_t>(i)].get<float>();
                }
            };
        }

        // Matrices are stored column-major, matching glm's own layout, so the
        // flat array can be memcpy-shaped in and out without transposing.
        template<typename MatT, int Columns, int Rows>
        Serializer::Writer matrixWriter() {
            return [](const void* instance) {
                const auto& value = *static_cast<const MatT*>(instance);
                nlohmann::json array = nlohmann::json::array();
                for (int column = 0; column < Columns; column++) {
                    for (int row = 0; row < Rows; row++) {
                        array.push_back(value[column][row]);
                    }
                }
                return array;
            };
        }

        template<typename MatT, int Columns, int Rows>
        Serializer::Reader matrixReader() {
            return [](void* instance, const nlohmann::json& json) {
                if (!json.is_array() || json.size() != Columns * Rows) {
                    return;
                }
                auto& value = *static_cast<MatT*>(instance);
                std::size_t index = 0;
                for (int column = 0; column < Columns; column++) {
                    for (int row = 0; row < Rows; row++) {
                        value[column][row] = json[index++].get<float>();
                    }
                }
            };
        }

        template<typename T>
        void registerScalar(Serializer& serializer) {
            serializer.registerLeaf<T>(
                [](const void* instance) {
                    return nlohmann::json(*static_cast<const T*>(instance));
                },
                [](void* instance, const nlohmann::json& json) {
                    // A wrong type in the file must not throw out of a scene
                    // load; leaving the default is the recoverable outcome.
                    if (json.is_null()) {
                        return;
                    }
                    try {
                        *static_cast<T*>(instance) = json.get<T>();
                    } catch (const nlohmann::json::exception& e) {
                        EGE_WARN("skipping value that does not convert: {}", e.what());
                    }
                });
        }

    }  // namespace

    Serializer& Serializer::instance() {
        static Serializer serializer;
        return serializer;
    }

    bool Serializer::hasLeafConverter(const TypeInfo& type) const {
        return converters.find(&type) != converters.end();
    }

    nlohmann::json Serializer::write(const TypeInfo& type, const void* instance) const {
        // A registered converter wins even for a type that also has fields, so
        // a project can choose a compact representation for something like a
        // quaternion.
        const auto converter = converters.find(&type);
        if (converter != converters.end()) {
            return converter->second.writer(instance);
        }

        if (type.isLeaf()) {
            // A reflected type with no fields is a tag: an empty object is the
            // correct and complete representation.
            if (type.isReflected()) {
                return nlohmann::json::object();
            }
            // Otherwise this is a primitive with no converter, which is a
            // missing registration. Warn, because silently writing null would
            // look like data loss much later.
            EGE_WARN("no serializer registered for leaf type '{}'", type.name());
            return nlohmann::json{};
        }

        nlohmann::json object = nlohmann::json::object();
        for (const FieldInfo& field : type.fields()) {
            object[std::string{field.name()}] = write(field.type(), field.addressIn(instance));
        }
        return object;
    }

    void Serializer::read(const TypeInfo& type, void* instance, const nlohmann::json& json) const {
        const auto converter = converters.find(&type);
        if (converter != converters.end()) {
            converter->second.reader(instance, json);
            return;
        }

        if (type.isLeaf() || !json.is_object()) {
            return;
        }

        for (const FieldInfo& field : type.fields()) {
            const auto found = json.find(std::string{field.name()});
            // Absent fields keep the instance's existing value, so a file
            // written by an older build still loads with new fields defaulted.
            if (found != json.end()) {
                read(field.type(), field.addressIn(instance), *found);
            }
        }
    }

    void registerBuiltinSerializers() {
        Serializer& serializer = Serializer::instance();

        registerScalar<bool>(serializer);
        registerScalar<std::int32_t>(serializer);
        registerScalar<std::int64_t>(serializer);
        registerScalar<std::uint32_t>(serializer);
        registerScalar<std::uint64_t>(serializer);
        registerScalar<float>(serializer);
        registerScalar<double>(serializer);
        registerScalar<std::string>(serializer);

        serializer.registerLeaf<glm::vec2>(
            vectorWriter<glm::vec2, 2>(), vectorReader<glm::vec2, 2>());
        serializer.registerLeaf<glm::vec3>(
            vectorWriter<glm::vec3, 3>(), vectorReader<glm::vec3, 3>());
        serializer.registerLeaf<glm::vec4>(
            vectorWriter<glm::vec4, 4>(), vectorReader<glm::vec4, 4>());
        serializer.registerLeaf<glm::mat3>(
            matrixWriter<glm::mat3, 3, 3>(), matrixReader<glm::mat3, 3, 3>());
        serializer.registerLeaf<glm::mat4>(
            matrixWriter<glm::mat4, 4, 4>(), matrixReader<glm::mat4, 4, 4>());
    }

}  // namespace ege
