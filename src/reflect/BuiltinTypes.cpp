#include "reflect/BuiltinTypes.hpp"

namespace ege {

    void registerBuiltinTypes() {
        registerType<bool>();
        registerType<char>();
        registerType<std::int8_t>();
        registerType<std::int16_t>();
        registerType<std::int32_t>();
        registerType<std::int64_t>();
        registerType<std::uint8_t>();
        registerType<std::uint16_t>();
        registerType<std::uint32_t>();
        registerType<std::uint64_t>();
        registerType<float>();
        registerType<double>();
        registerType<std::string>();

        registerType<glm::vec2>();
        registerType<glm::vec3>();
        registerType<glm::vec4>();
        registerType<glm::mat3>();
        registerType<glm::mat4>();
    }

}  // namespace ege
