# Warning configuration shared by every engine target.
#
# ege_set_target_warnings(<target> [WARNINGS_AS_ERRORS])
#
# Kept in one place so the engine, editor and runtime targets cannot drift
# apart, and so third-party code pulled in as a dependency is never subjected
# to our warning set.

function(ege_set_target_warnings target)
  cmake_parse_arguments(ARG "WARNINGS_AS_ERRORS" "" "" ${ARGN})

  set(MSVC_WARNINGS
      /W4              # reasonable and standard
      /w14242          # conversion, possible loss of data
      /w14254          # larger bit field assigned to smaller
      /w14263          # member function hides a virtual, but does not override
      /w14265          # class has virtual functions but a non-virtual destructor
      /w14287          # unsigned/negative constant mismatch
      /we4289          # loop control variable used outside the loop
      /w14296          # comparison is always true/false
      /w14311          # pointer truncation
      /w14545          # expression before comma has no effect
      /w14546          # function call before comma missing argument list
      /w14547          # operator before comma has no effect
      /w14549          # operator before comma has no effect
      /w14555          # expression has no effect
      /w14619          # pragma warning for a warning number that does not exist
      /w14640          # thread-unsafe static member initialization
      /w14826          # sign-extending conversion, possible behaviour change
      /w14905          # wide string literal cast to LPSTR
      /w14906          # string literal cast to LPWSTR
      /w14928          # illegal copy-initialization
      /permissive-     # standards conformance
  )

  set(CLANG_WARNINGS
      -Wall
      -Wextra
      -Wshadow                # a declaration shadows an outer one
      -Wnon-virtual-dtor      # polymorphic class with a non-virtual destructor
      -Wold-style-cast        # C-style casts
      -Wcast-align            # potentially performance-costly cast
      -Wunused
      -Woverloaded-virtual    # overload, not override, of a virtual
      -Wpedantic
      -Wconversion            # type conversions that may lose data
      -Wsign-conversion
      -Wnull-dereference
      -Wdouble-promotion      # implicit float to double
      -Wformat=2
  )

  set(GCC_WARNINGS
      ${CLANG_WARNINGS}
      -Wmisleading-indentation
      -Wduplicated-cond
      -Wduplicated-branches
      -Wlogical-op
      -Wuseless-cast
  )

  if(ARG_WARNINGS_AS_ERRORS)
    list(APPEND MSVC_WARNINGS /WX)
    list(APPEND CLANG_WARNINGS -Werror)
    list(APPEND GCC_WARNINGS -Werror)
  endif()

  if(MSVC)
    set(WARNINGS ${MSVC_WARNINGS})
  elseif(CMAKE_CXX_COMPILER_ID MATCHES ".*Clang")
    set(WARNINGS ${CLANG_WARNINGS})
  elseif(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    set(WARNINGS ${GCC_WARNINGS})
  else()
    message(AUTHOR_WARNING "No warning set defined for ${CMAKE_CXX_COMPILER_ID}")
  endif()

  # PRIVATE so consumers of the target are never forced into our warning set.
  target_compile_options(${target} PRIVATE ${WARNINGS})
endfunction()
