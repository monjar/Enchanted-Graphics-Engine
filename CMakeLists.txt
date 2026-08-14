cmake_minimum_required(VERSION 3.21)

project(Enchanted
  VERSION 0.1.0
  DESCRIPTION "Enchanted Graphics Engine"
  LANGUAGES CXX)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
  set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS
    Debug Release RelWithDebInfo MinSizeRel)
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")

option(EGE_BUILD_TESTS "Build the test suite" ON)

include(CompilerWarnings)
include(Dependencies)
include(Shaders)

# ---------------------------------------------------------------------------
# Enchanted - the engine, as a library
#
# Everything except the entry point lives here, so that the application, the
# tests and eventually the editor all link the same code rather than each
# recompiling it.
# ---------------------------------------------------------------------------
file(GLOB_RECURSE ENGINE_SOURCES CONFIGURE_DEPENDS
  "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp")

add_library(Enchanted STATIC ${ENGINE_SOURCES})
add_library(ege::engine ALIAS Enchanted)

target_include_directories(Enchanted PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")

target_link_libraries(Enchanted PUBLIC
  Vulkan::Vulkan
  ege::glfw
  ege::glm
  ege::tinyobj
  ege::cgltf
  ege::imgui
  ege::imguizmo
  ege::spdlog
  ege::vma
  ege::stb
  ege::json
  ege::jolt)

target_compile_definitions(Enchanted PRIVATE
  EGE_ASSET_ROOT="${CMAKE_CURRENT_SOURCE_DIR}/assets")

ege_set_target_warnings(Enchanted WARNINGS_AS_ERRORS)

ege_add_shaders(Enchanted "${CMAKE_CURRENT_SOURCE_DIR}/shaders")

# ---------------------------------------------------------------------------
# EnchantedEngine - the application
# ---------------------------------------------------------------------------
add_executable(EnchantedEngine app/main.cpp)
target_link_libraries(EnchantedEngine PRIVATE ege::engine)
ege_set_target_warnings(EnchantedEngine WARNINGS_AS_ERRORS)

set_property(TARGET EnchantedEngine
  PROPERTY VS_DEBUGGER_WORKING_DIRECTORY "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}")

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------
if(EGE_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()
