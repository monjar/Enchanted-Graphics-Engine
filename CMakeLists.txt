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

# Everything next to the executables. On Windows a DLL is looked for beside
# the binary that loads it and nowhere useful otherwise; on Linux the build
# tree's RPATH already handles it, and having them in one place makes the two
# platforms behave the same way.
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")

# The dependencies are static libraries and are about to be linked into a
# shared one, which on ELF requires every object in them to be
# position-independent.
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# Find the engine library beside the binary that loaded it, by a path relative
# to that binary rather than an absolute one baked in at link time. It is what
# lets the build tree's bin/ directory be moved or copied and still run, and
# it is what a script module needs: the module is opened by the executable, so
# @loader_path resolves against the executable, and both of them are in the
# same directory as the engine.
if(APPLE)
  set(CMAKE_BUILD_RPATH "@loader_path")
  set(CMAKE_INSTALL_RPATH "@loader_path")
else()
  set(CMAKE_BUILD_RPATH "$ORIGIN")
  set(CMAKE_INSTALL_RPATH "$ORIGIN")
endif()

# MSVC exports nothing from a DLL unless it is asked to, and asking for it
# one __declspec at a time across an engine's worth of classes is a
# maintenance burden with no upside. This exports the functions
# automatically. It does not export data symbols - which is why the type
# registry is reached through a function rather than being one.
set(CMAKE_WINDOWS_EXPORT_ALL_SYMBOLS ON)

option(EGE_BUILD_TESTS "Build the test suite" ON)

# Shared by default, and the reason is script hot reload rather than binary
# size. A script module loaded with dlopen or LoadLibrary links against the
# engine; if the engine is a static library, the module gets its own copy of
# every registry inside it - the type registry, the component registry, the
# behaviour registry - and nothing it registers is visible to the engine that
# loaded it. One shared engine is what makes those registries one registry.
#
# Left as an option because a static build is still the right answer for
# anything shipping a single self-contained binary and not loading modules.
option(EGE_SHARED_ENGINE "Build the engine as a shared library" ON)

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

if(EGE_SHARED_ENGINE)
  add_library(Enchanted SHARED ${ENGINE_SOURCES})
else()
  add_library(Enchanted STATIC ${ENGINE_SOURCES})
endif()
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
  ege::jolt
  ege::miniaudio)

target_compile_definitions(Enchanted PRIVATE
  EGE_ASSET_ROOT="${CMAKE_CURRENT_SOURCE_DIR}/assets"
  # Where script modules are built to, so a development run finds the sandbox
  # without being told. A shipped game would look beside its own executable;
  # this is the same trick EGE_ASSET_ROOT plays, for the same reason.
  EGE_MODULE_ROOT="${CMAKE_RUNTIME_OUTPUT_DIRECTORY}")

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
# EnchantedSandbox - a project's behaviours, as a module the engine loads
#
# MODULE rather than SHARED: nothing links against this, it is opened by name
# at runtime. Nothing in the engine names anything in it either - a scene
# refers to a behaviour by the name it registered under, and whether that name
# resolves is a question of what has been loaded.
#
# It exists to be rebuilt while the engine is running, which is what script
# hot reload is: `cmake --build --preset default --target EnchantedSandbox`
# with the demo up, and the running scene picks the new behaviour up.
# ---------------------------------------------------------------------------
if(EGE_SHARED_ENGINE)
  file(GLOB_RECURSE SANDBOX_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/sandbox/*.cpp")

  add_library(EnchantedSandbox MODULE ${SANDBOX_SOURCES})
  target_link_libraries(EnchantedSandbox PRIVATE ege::engine)
  ege_set_target_warnings(EnchantedSandbox WARNINGS_AS_ERRORS)

  # Beside the executable that loads it, under whatever name the platform
  # gives a module - libEnchantedSandbox.so, EnchantedSandbox.dll. The engine
  # composes the same name from the same base, which is the one rule both
  # sides have to agree on.
  set_target_properties(EnchantedSandbox PROPERTIES
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}")

  # Except on macOS, where CMake gives a MODULE library a .so suffix - a
  # holdover from when a loadable bundle was distinct from a dylib, and not
  # what anything on the platform calls a dynamic library today. The engine
  # asks for lib<name>.dylib there, so left alone the two sides would compose
  # different names and the module would simply never be found. Said here
  # rather than changed there because .dylib is what a Mac developer will look
  # for in the build directory.
  if(APPLE)
    set_target_properties(EnchantedSandbox PROPERTIES SUFFIX ".dylib")
  endif()

  # Built by default, so a fresh checkout has something to load; the demo
  # still runs without it, with the behaviours it names simply absent.
  add_dependencies(EnchantedEngine EnchantedSandbox)
endif()

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

# It has to go on whichever binary actually imports the loader, and making
# the engine shared moved that. It used to be each executable, because each
# linked the engine's objects and inherited its imports; now the imports are
# in the DLL and the executables have none, so a /DELAYLOAD on them is a
# no-op the linker warns about - and the DLL, bound at load time, would drag
# vulkan-1.dll in before a single line of any of them ran. On a machine
# without the loader that is the process failing to start rather than a test
# suite running: which is exactly what happened, as STATUS_DLL_NOT_FOUND out
# of the test enumeration step.
ege_delay_load_vulkan(Enchanted)
if(NOT EGE_SHARED_ENGINE)
  ege_delay_load_vulkan(EnchantedEngine)
  # The frame checker links the engine and so imports the loader, but never
  # makes a Vulkan call - it reads PNGs.
  ege_delay_load_vulkan(EnchantedFrameChecks)
endif()

set_property(TARGET EnchantedEngine
  PROPERTY VS_DEBUGGER_WORKING_DIRECTORY "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}")

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------
if(EGE_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()
