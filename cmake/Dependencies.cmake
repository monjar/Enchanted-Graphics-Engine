# Third-party dependency resolution.
#
# Every dependency except the Vulkan SDK is declared here through CPM, which
# tries find_package() first and only downloads a pinned source tree when the
# system does not already provide the package. That keeps distribution packages
# working where they exist while still letting a clean checkout configure on a
# machine with nothing installed but a compiler, CMake and the Vulkan SDK.
#
# The Vulkan SDK stays a find_package: it ships loader libraries and validation
# layers that have to match the installed driver, so vendoring it is wrong.

include(CPM)

# Re-expose a dependency's interface include directories as SYSTEM.
#
# A package resolved by find_package usually marks its includes SYSTEM already,
# but one built from source through CPM does not, which puts third-party
# headers under our warning set. With warnings as errors that turns any
# upstream sloppiness into a build failure in our tree.
function(ege_make_includes_system target)
  if(NOT TARGET ${target})
    return()
  endif()
  get_target_property(aliased ${target} ALIASED_TARGET)
  if(aliased)
    set(target ${aliased})
  endif()
  get_target_property(includes ${target} INTERFACE_INCLUDE_DIRECTORIES)
  if(includes)
    set_target_properties(${target} PROPERTIES
      INTERFACE_SYSTEM_INCLUDE_DIRECTORIES "${includes}")
  endif()
endfunction()

# ---------------------------------------------------------------------------
# Vulkan - required, must come from the system
# ---------------------------------------------------------------------------
find_package(Vulkan REQUIRED)
message(STATUS "Vulkan: ${Vulkan_LIBRARIES} (${Vulkan_VERSION})")

# ---------------------------------------------------------------------------
# GLFW - windowing and input
# ---------------------------------------------------------------------------
set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(GLFW_INSTALL OFF CACHE BOOL "" FORCE)

CPMAddPackage(
  NAME glfw
  GITHUB_REPOSITORY glfw/glfw
  GIT_TAG 3.4
  VERSION 3.4
  FIND_PACKAGE_ARGUMENTS "NAMES glfw3"
  EXCLUDE_FROM_ALL YES
)

# find_package(glfw3) exports the target as glfw; a source build defines it
# directly. Normalise so callers always link ege::glfw.
if(TARGET glfw)
  add_library(ege_glfw INTERFACE)
  target_link_libraries(ege_glfw INTERFACE glfw)
elseif(TARGET glfw3)
  add_library(ege_glfw INTERFACE)
  target_link_libraries(ege_glfw INTERFACE glfw3)
else()
  message(FATAL_ERROR "GLFW resolved but exported no usable target")
endif()
ege_make_includes_system(glfw)
ege_make_includes_system(glfw3)
add_library(ege::glfw ALIAS ege_glfw)

# ---------------------------------------------------------------------------
# GLM - maths. Header only.
# ---------------------------------------------------------------------------
CPMAddPackage(
  NAME glm
  GITHUB_REPOSITORY g-truc/glm
  GIT_TAG 1.0.1
  VERSION 1.0.1
  FIND_PACKAGE_ARGUMENTS "NAMES glm"
  EXCLUDE_FROM_ALL YES
)

add_library(ege_glm INTERFACE)
if(glm_ADDED)
  # Built from source: use the source tree directly so it can be marked SYSTEM.
  target_include_directories(ege_glm SYSTEM INTERFACE ${glm_SOURCE_DIR})
elseif(TARGET glm::glm)
  ege_make_includes_system(glm::glm)
  target_link_libraries(ege_glm INTERFACE glm::glm)
else()
  # Older distribution packages ship headers with no CMake config at all.
  find_path(GLM_INCLUDE_DIR glm/glm.hpp REQUIRED)
  target_include_directories(ege_glm SYSTEM INTERFACE ${GLM_INCLUDE_DIR})
endif()
add_library(ege::glm ALIAS ege_glm)

# GLM is configured by macros that must be identical in every translation unit,
# so they travel with the target rather than being repeated per file.
#   GLM_FORCE_RADIANS           - angle arguments are radians, never degrees
#   GLM_FORCE_DEPTH_ZERO_TO_ONE - Vulkan clip space is [0, 1], not OpenGL [-1, 1]
#   GLM_ENABLE_EXPERIMENTAL     - required by the glm/gtx headers
target_compile_definitions(ege_glm INTERFACE
  GLM_FORCE_RADIANS
  GLM_FORCE_DEPTH_ZERO_TO_ONE
  GLM_ENABLE_EXPERIMENTAL)

# ---------------------------------------------------------------------------
# tinyobjloader - OBJ import. Vendored in-tree as a single header.
# ---------------------------------------------------------------------------
add_library(ege_tinyobj INTERFACE)
target_include_directories(ege_tinyobj SYSTEM INTERFACE
  ${CMAKE_CURRENT_SOURCE_DIR}/external/tinyobjectloader)
add_library(ege::tinyobj ALIAS ege_tinyobj)

# ---------------------------------------------------------------------------
# Dear ImGui (docking branch) - the editor UI. Compiled as its own static
# library with the GLFW and Vulkan backends, and without the engine's
# warnings-as-errors, because third-party code is not ours to fix.
# ---------------------------------------------------------------------------
CPMAddPackage(
  NAME imgui
  GITHUB_REPOSITORY ocornut/imgui
  GIT_TAG v1.91.8-docking
  VERSION 1.91.8
  EXCLUDE_FROM_ALL YES
  DOWNLOAD_ONLY YES
)

add_library(ege_imgui STATIC
  ${imgui_SOURCE_DIR}/imgui.cpp
  ${imgui_SOURCE_DIR}/imgui_demo.cpp
  ${imgui_SOURCE_DIR}/imgui_draw.cpp
  ${imgui_SOURCE_DIR}/imgui_tables.cpp
  ${imgui_SOURCE_DIR}/imgui_widgets.cpp
  ${imgui_SOURCE_DIR}/backends/imgui_impl_glfw.cpp
  ${imgui_SOURCE_DIR}/backends/imgui_impl_vulkan.cpp)
target_include_directories(ege_imgui SYSTEM PUBLIC
  ${imgui_SOURCE_DIR}
  ${imgui_SOURCE_DIR}/backends)
target_link_libraries(ege_imgui PUBLIC ege::glfw Vulkan::Vulkan)
add_library(ege::imgui ALIAS ege_imgui)

# ---------------------------------------------------------------------------
# cgltf - glTF 2.0 import. Single header, implementation compiled in
# assets/GltfLoader.cpp.
# ---------------------------------------------------------------------------
CPMAddPackage(
  NAME cgltf
  GITHUB_REPOSITORY jkuhlmann/cgltf
  GIT_TAG v1.14
  VERSION 1.14
  EXCLUDE_FROM_ALL YES
  DOWNLOAD_ONLY YES
)

add_library(ege_cgltf INTERFACE)
target_include_directories(ege_cgltf SYSTEM INTERFACE ${cgltf_SOURCE_DIR})
add_library(ege::cgltf ALIAS ege_cgltf)

# ---------------------------------------------------------------------------
# spdlog - logging
# ---------------------------------------------------------------------------
set(SPDLOG_BUILD_EXAMPLE OFF CACHE BOOL "" FORCE)
set(SPDLOG_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(SPDLOG_INSTALL OFF CACHE BOOL "" FORCE)

CPMAddPackage(
  NAME spdlog
  GITHUB_REPOSITORY gabime/spdlog
  GIT_TAG v1.14.1
  VERSION 1.14.1
  FIND_PACKAGE_ARGUMENTS "NAMES spdlog"
  EXCLUDE_FROM_ALL YES
)

add_library(ege_spdlog INTERFACE)
if(TARGET spdlog::spdlog)
  ege_make_includes_system(spdlog::spdlog)
  target_link_libraries(ege_spdlog INTERFACE spdlog::spdlog)
elseif(TARGET spdlog)
  ege_make_includes_system(spdlog)
  target_link_libraries(ege_spdlog INTERFACE spdlog)
else()
  message(FATAL_ERROR "spdlog resolved but exported no usable target")
endif()
add_library(ege::spdlog ALIAS ege_spdlog)

# ---------------------------------------------------------------------------
# VulkanMemoryAllocator - GPU memory suballocation
# ---------------------------------------------------------------------------
CPMAddPackage(
  NAME VulkanMemoryAllocator
  GITHUB_REPOSITORY GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator
  GIT_TAG v3.1.0
  VERSION 3.1.0
  EXCLUDE_FROM_ALL YES
  DOWNLOAD_ONLY YES
)

add_library(ege_vma INTERFACE)
target_include_directories(ege_vma SYSTEM INTERFACE
  ${VulkanMemoryAllocator_SOURCE_DIR}/include)
add_library(ege::vma ALIAS ege_vma)

# ---------------------------------------------------------------------------
# stb - image loading. Header only, implementation compiled in one TU.
# ---------------------------------------------------------------------------
CPMAddPackage(
  NAME stb
  GITHUB_REPOSITORY nothings/stb
  GIT_TAG f0569113c93ad095470c54bf34a17b36646bbbb5
  EXCLUDE_FROM_ALL YES
  DOWNLOAD_ONLY YES
)

add_library(ege_stb INTERFACE)
target_include_directories(ege_stb SYSTEM INTERFACE ${stb_SOURCE_DIR})
add_library(ege::stb ALIAS ege_stb)

# ---------------------------------------------------------------------------
# nlohmann/json - scene and asset serialization
# ---------------------------------------------------------------------------
set(JSON_BuildTests OFF CACHE INTERNAL "")
set(JSON_Install OFF CACHE INTERNAL "")

CPMAddPackage(
  NAME nlohmann_json
  GITHUB_REPOSITORY nlohmann/json
  GIT_TAG v3.11.3
  VERSION 3.11.3
  FIND_PACKAGE_ARGUMENTS "NAMES nlohmann_json"
  EXCLUDE_FROM_ALL YES
)

add_library(ege_json INTERFACE)
if(TARGET nlohmann_json::nlohmann_json)
  ege_make_includes_system(nlohmann_json::nlohmann_json)
  target_link_libraries(ege_json INTERFACE nlohmann_json::nlohmann_json)
else()
  message(FATAL_ERROR "nlohmann_json resolved but exported no usable target")
endif()
add_library(ege::json ALIAS ege_json)
