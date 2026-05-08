/*
 * t3_csprng_windows.c — Windows CSPRNG backend.
 *
 * Uses BCryptGenRandom(NULL, buf, len, BCRYPT_USE_SYSTEM_PREFERRED_RNG)
 * from bcrypt.lib (linked via CMakeLists.txt on WIN32).  The
 * BCRYPT_USE_SYSTEM_PREFERRED_RNG flag delegates algorithm selection to
 * the kernel; non-blocking.  FIPS posture is determined by OS configuration
 * (FIPS-mode policy at the host level) — not asserted here.
 *
 * CMake adds: target_link_libraries(teleproto3 PRIVATE bcrypt) on WIN32.
 *
 * Story 1-14 implementation.
 */

#if !defined(_WIN32)
#error "t3_csprng_windows.c must only be compiled on Windows (_WIN32 not defined)"
#endif

#include <windows.h>
#include <bcrypt.h>
#include <limits.h>
#include <stdint.h>
#include <stddef.h>

#include "t3.h"
#include "t3_csprng.h"

t3_result_t t3_csprng_bytes(uint8_t *buf, size_t len) {
    if (len == 0) return T3_OK;

    /*
     * BCryptGenRandom takes a ULONG count.  On 64-bit Windows size_t can
     * exceed ULONG_MAX, so we loop in ULONG-sized chunks.  In practice
     * callers never request multi-GB buffers, but the guard is required
     * by the spec (t3_csprng.h: len is size_t).
     */
    uint8_t *p   = buf;
    size_t   rem = len;

    while (rem > 0) {
        ULONG chunk = (rem > (size_t)ULONG_MAX) ? ULONG_MAX : (ULONG)rem;

        NTSTATUS status = BCryptGenRandom(
            NULL,
            (PUCHAR)p,
            chunk,
            BCRYPT_USE_SYSTEM_PREFERRED_RNG
        );

        /*
         * BCRYPT_SUCCESS expands to ((NTSTATUS)(s) >= 0); accepts both
         * STATUS_SUCCESS and any future STATUS_INFORMATIONAL codes,
         * matching NT_SUCCESS convention.
         */
        if (!BCRYPT_SUCCESS(status)) {
            return T3_ERR_RNG;
        }

        p   += chunk;
        rem -= chunk;
    }

    return T3_OK;
}
