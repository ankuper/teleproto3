/*
 * t3_strerror.c — human-readable result code strings.
 *
 * This is the reference implementation of the Type3 protocol.
 * Normative behaviour is defined in spec/. Where they differ, spec/ wins.
 *
 * Generated from T3_RESULT_LIST X-macro. Source: story 1.7 Task 6.
 * Stability: lib-v0.1.0 ABI (frozen by story 1.6).
 */

#include "t3.h"

T3_API const char *t3_strerror(t3_result_t rc) {
#define T3_STRERROR_CASE(name, value, msg) case name: return (msg);
    switch (rc) {
        T3_RESULT_LIST(T3_STRERROR_CASE)
        default: return "unknown error";
    }
#undef T3_STRERROR_CASE
}
