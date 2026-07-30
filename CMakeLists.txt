cmake_minimum_required(VERSION 3.21)

project(Enchanted
  VERSION 0.1.0
  DESCRIPTION "Enchanted Graphics Engine"
  LANGUAGES CXX)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

# Emit compile_commands.json for clangd, clang-tidy and editor tooling.
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Default to Release so an unqualified `cmake -S . -B build` does not silently
# produce an unoptimised binary.
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
  set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS
    Debug Release RelWithDebInfo MinSizeRel)
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")

include(CompilerWarnings)
include(Dependencies)
include(Shaders)

# ---------------------------------------------------------------------------
# Engine executable
# ---------------------------------------------------------------------------
file(GLOB_RECURSE ENGINE_SOURCES CONFIGURE_DEPENDS
  "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp")

add_executable(EnchantedEngine ${ENGINE_SOURCES})

target_include_directories(EnchantedEngine PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")

target_link_libraries(EnchantedEngine PRIVATE
  Vulkan::Vulkan
  ege::glfw
  ege::glm
  ege::tinyobj)

target_compile_definitions(EnchantedEngine PRIVATE
  EGE_ASSET_ROOT="${CMAKE_CURRENT_SOURCE_DIR}/assets")

ege_set_target_warnings(EnchantedEngine WARNINGS_AS_ERRORS)

# Compiles the shaders, makes them a build dependency, and defines
# EGE_SHADER_ROOT on the target.
ege_add_shaders(EnchantedEngine "${CMAKE_CURRENT_SOURCE_DIR}/shaders")

# Visual Studio otherwise launches the debugger from the build directory.
set_property(TARGET EnchantedEngine
  PROPERTY VS_DEBUGGER_WORKING_DIRECTORY "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}")
