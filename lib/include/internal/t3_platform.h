/*
 * t3_platform.h — internal platform-portability macros.
 *
 * This is the reference implementation of the Type3 protocol.
 * Normative behaviour is defined in spec/. Where they differ, spec/ wins.
 *
 * Stability: internal header; NOT exported; NOT ABI-stable.
 *
 * T3_HIDDEN
 *   Internal-symbol visibility marker. Expands to
 *   __attribute__((visibility("hidden"))) on GCC/Clang/AppleClang and to
 *   nothing on MSVC (Windows static archives don't expose internal symbols
 *   regardless, and the GCC attribute syntax errors under /W4 /WX).
 *   Apply to non-T3_API declarations and definitions inside lib/.
 */

#ifndef T3_INTERNAL_T3_PLATFORM_H
#define T3_INTERNAL_T3_PLATFORM_H

#if defined(__GNUC__) || defined(__clang__)
#  define T3_HIDDEN __attribute__((visibility("hidden")))
#else
#  define T3_HIDDEN
#endif

#endif /* T3_INTERNAL_T3_PLATFORM_H */
