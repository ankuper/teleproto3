/*
 * t3_abi_version.c — runtime ABI version string.
 *
 * This is the reference implementation of the Type3 protocol.
 * Normative behaviour is defined in spec/. Where they differ, spec/ wins.
 *
 * Source: story 1.7 Task 6.2. Stability: lib-v0.1.0 ABI.
 */

#include "t3.h"

T3_API const char *t3_abi_version_string(void) {
    return T3_INTERNAL_STR(T3_ABI_VERSION_MAJOR) "."
           T3_INTERNAL_STR(T3_ABI_VERSION_MINOR) "."
           T3_INTERNAL_STR(T3_ABI_VERSION_PATCH);
}
