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

if(TARGET glm::glm)
  add_library(ege_glm INTERFACE)
  target_link_libraries(ege_glm INTERFACE glm::glm)
elseif(glm_ADDED)
  add_library(ege_glm INTERFACE)
  target_include_directories(ege_glm SYSTEM INTERFACE ${glm_SOURCE_DIR})
else()
  # Older distribution packages ship headers with no CMake config at all.
  find_path(GLM_INCLUDE_DIR glm/glm.hpp REQUIRED)
  add_library(ege_glm INTERFACE)
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
