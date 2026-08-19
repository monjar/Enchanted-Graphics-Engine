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

# GCC's -Wtsan flags Jolt's fence-based reference counting when it is inlined
# into this one translation unit, and SYSTEM include suppression does not
# reach warnings raised at inlining time. The warning is about Jolt's idiom,
# not our code, so it is switched off for exactly that file - everywhere,
# not only under TSan, because the flag is harmless when TSan is off and the
# preset must not change which warnings exist.
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
  set_source_files_properties(src/physics/JoltPhysicsWorld.cpp
    PROPERTIES COMPILE_OPTIONS "-Wno-tsan")
endif()

ege_add_shaders(Enchanted "${CMAKE_CURRENT_SOURCE_DIR}/shaders")

# ---------------------------------------------------------------------------
# EnchantedEngine - the application
# ---------------------------------------------------------------------------
add_executable(EnchantedEngine app/main.cpp)
target_link_libraries(EnchantedEngine PRIVATE ege::engine)
ege_set_target_warnings(EnchantedEngine WARNINGS_AS_ERRORS)

# ---------------------------------------------------------------------------
# EnchantedFrameChecks - looks at recorded frames and says whether a picture
# came out.
#
# Not part of the test suite, because the suite deliberately never touches a
# GPU and these frames only exist once one has rendered them. It runs in CI's
# headless render step instead, on the frames the engine recorded there. The
# logic it calls lives in the engine library and is unit-tested without any
# of this.
# ---------------------------------------------------------------------------
add_executable(EnchantedFrameChecks tools/frame_checks.cpp)
target_link_libraries(EnchantedFrameChecks PRIVATE ege::engine ege::stb)
ege_set_target_warnings(EnchantedFrameChecks WARNINGS_AS_ERRORS)

# On Windows the Vulkan loader is delay-loaded rather than bound at process
# start. A machine without the loader - a CI runner with no GPU driver, say -
# can then still start the binary and run everything up to the first actual
# Vulkan call, which is what lets the test suite run on such a machine at
# all: LunarG ships the loader DLL through the SDK installer's runtime
# component, and CI only extracts the SDK tree.
function(ege_delay_load_vulkan target)
  if(MSVC)
    target_link_options(${target} PRIVATE "/DELAYLOAD:vulkan-1.dll")
    target_link_libraries(${target} PRIVATE delayimp)
  endif()
endfunction()

ege_delay_load_vulkan(EnchantedEngine)
# The frame checker links the engine and so imports the loader, but never
# makes a Vulkan call - it reads PNGs.
ege_delay_load_vulkan(EnchantedFrameChecks)

set_property(TARGET EnchantedEngine
  PROPERTY VS_DEBUGGER_WORKING_DIRECTORY "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}")

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------
if(EGE_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()
