# GLSL -> SPIR-V compilation.
#
# ege_add_shaders(<target> <shader_dir>)
#
# Compiles every .vert/.frag/.comp/.geom/.tesc/.tese under <shader_dir> into
# ${CMAKE_BINARY_DIR}/shaders, makes the result a build dependency of <target>,
# and defines EGE_SHADER_ROOT on <target> so lookup at runtime does not depend
# on the working directory.
#
# Originally adapted from vblanco20-1's vulkan-guide.

find_program(EGE_GLSL_VALIDATOR
  NAMES glslangValidator
  HINTS
    ${Vulkan_GLSLANG_VALIDATOR_EXECUTABLE}
    $ENV{VULKAN_SDK}/bin
    $ENV{VULKAN_SDK}/Bin
    $ENV{VULKAN_SDK}/Bin32
    /usr/bin
    /usr/local/bin
    # Homebrew on Apple Silicon, which is not on any default search path and
    # is where `brew install glslang` puts this on every Mac sold since 2020.
    /opt/homebrew/bin
  DOC "glslangValidator, used to compile GLSL to SPIR-V")

function(ege_add_shaders target shader_dir)
  if(NOT EGE_GLSL_VALIDATOR)
    message(FATAL_ERROR
      "glslangValidator was not found. Install the Vulkan SDK, or on Debian and "
      "Ubuntu the glslang-tools package.")
  endif()

  file(GLOB_RECURSE GLSL_SOURCES CONFIGURE_DEPENDS
    "${shader_dir}/*.vert"
    "${shader_dir}/*.frag"
    "${shader_dir}/*.comp"
    "${shader_dir}/*.geom"
    "${shader_dir}/*.tesc"
    "${shader_dir}/*.tese")

  if(NOT GLSL_SOURCES)
    message(FATAL_ERROR "No shader sources found under ${shader_dir}")
  endif()

  # Shared declarations - the global uniform block above all - live in .glsl
  # files that are #included rather than compiled on their own. They are not
  # in the glob for that reason, but every stage has to be rebuilt when one
  # changes, so they are named as dependencies of all of them.
  file(GLOB_RECURSE GLSL_INCLUDES CONFIGURE_DEPENDS "${shader_dir}/*.glsl")

  # Compiled SPIR-V is a build artifact, so it goes in the build tree rather
  # than beside the GLSL sources where it would be easy to commit by accident.
  set(output_dir "${CMAKE_BINARY_DIR}/shaders")

  set(spirv_outputs "")
  foreach(glsl ${GLSL_SOURCES})
    get_filename_component(name ${glsl} NAME)
    set(spirv "${output_dir}/${name}.spv")
    add_custom_command(
      OUTPUT ${spirv}
      COMMAND ${CMAKE_COMMAND} -E make_directory ${output_dir}
      COMMAND ${EGE_GLSL_VALIDATOR} -V -I${shader_dir} ${glsl} -o ${spirv}
      DEPENDS ${glsl} ${GLSL_INCLUDES}
      COMMENT "Compiling shader ${name}"
      VERBATIM)
    list(APPEND spirv_outputs ${spirv})
  endforeach()

  add_custom_target(${target}_Shaders DEPENDS ${spirv_outputs})
  add_dependencies(${target} ${target}_Shaders)

  target_compile_definitions(${target} PRIVATE EGE_SHADER_ROOT="${output_dir}")
endfunction()
