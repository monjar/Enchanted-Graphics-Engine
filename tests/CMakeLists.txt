# Test suite.
#
# These link the engine library but must not require a Vulkan device: they
# cover the pure logic - geometry generation, transform maths, buffer
# alignment - so they run anywhere, including CI machines with no GPU.

CPMAddPackage(
  NAME doctest
  GITHUB_REPOSITORY doctest/doctest
  GIT_TAG v2.4.11
  VERSION 2.4.11
  EXCLUDE_FROM_ALL YES
)
ege_make_includes_system(doctest)
ege_make_includes_system(doctest_with_main)

file(GLOB TEST_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp")

add_executable(EnchantedTests ${TEST_SOURCES})

target_link_libraries(EnchantedTests PRIVATE ege::engine doctest::doctest)

# doctest forward-declares std::ostream rather than including <ostream>, and
# MSVC's <string_view> stream inserter cannot be instantiated against a
# forward-declared basic_ostream - so any CHECK that stringifies a
# string_view fails to compile on Windows. This is doctest's own switch for
# exactly that: include the real standard headers instead.
target_compile_definitions(EnchantedTests PRIVATE DOCTEST_CONFIG_USE_STD_HEADERS)

ege_set_target_warnings(EnchantedTests WARNINGS_AS_ERRORS)

include("${doctest_SOURCE_DIR}/scripts/cmake/doctest.cmake")
doctest_discover_tests(EnchantedTests)
