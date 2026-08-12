#pragma once

#include "reflect/Reflect.hpp"

#include <glm/glm.hpp>

// Names for the leaf types the inspector has to draw directly.
//
// These have no fields to reflect - a float is a float - but they still need a
// presentable name, because the inspector selects a drawing widget by type and
// the editor shows the type beside the field.

EGE_TYPE_NAME(bool, "bool")
EGE_TYPE_NAME(char, "char")
EGE_TYPE_NAME(std::int8_t, "int8")
EGE_TYPE_NAME(std::int16_t, "int16")
EGE_TYPE_NAME(std::int32_t, "int32")
EGE_TYPE_NAME(std::int64_t, "int64")
EGE_TYPE_NAME(std::uint8_t, "uint8")
EGE_TYPE_NAME(std::uint16_t, "uint16")
EGE_TYPE_NAME(std::uint32_t, "uint32")
EGE_TYPE_NAME(std::uint64_t, "uint64")
EGE_TYPE_NAME(float, "float")
EGE_TYPE_NAME(double, "double")
EGE_TYPE_NAME(std::string, "string")

EGE_TYPE_NAME(glm::vec2, "vec2")
EGE_TYPE_NAME(glm::vec3, "vec3")
EGE_TYPE_NAME(glm::vec4, "vec4")
EGE_TYPE_NAME(glm::mat3, "mat3")
EGE_TYPE_NAME(glm::mat4, "mat4")

namespace ege {

    // Forces the built-in types into the registry, so lookup by name works
    // before anything has touched them. Called during engine start-up.
    void registerBuiltinTypes();

}  // namespace ege
