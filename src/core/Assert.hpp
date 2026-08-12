#pragma once

#include "core/Log.hpp"

#include <cstdlib>

// Assertions that log before they abort.
//
// The codebase previously used bare assert(), which prints to stderr with no
// context and compiles out entirely under NDEBUG - so every invariant the
// renderer relied on was unchecked in exactly the build that ships.
//
//   EGE_ASSERT(cond, "message {}", arg)  Debug only. Compiled out in release.
//   EGE_VERIFY(cond, "message {}", arg)  Always checked, in every build.
//
// Use EGE_VERIFY for anything whose failure would corrupt state or read out of
// bounds; EGE_ASSERT for invariants that are expensive to check or that only
// catch programmer error during development.

#define EGE_INTERNAL_CHECK(condition, ...)                        \
    do {                                                          \
        if (!(condition)) {                                       \
            if (::ege::Log::initialised()) {                      \
                EGE_CRITICAL("Assertion failed: {}", #condition); \
                EGE_CRITICAL("  at {}:{}", __FILE__, __LINE__);   \
                EGE_CRITICAL("  " __VA_ARGS__);                   \
                ::ege::Log::engine().flush();                     \
            }                                                     \
            std::abort();                                         \
        }                                                         \
    } while (false)

#define EGE_VERIFY(condition, ...) EGE_INTERNAL_CHECK(condition, __VA_ARGS__)

#ifdef NDEBUG
#define EGE_ASSERT(condition, ...) ((void)0)
#else
#define EGE_ASSERT(condition, ...) EGE_INTERNAL_CHECK(condition, __VA_ARGS__)
#endif
