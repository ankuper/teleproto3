/*
 * t3_csprng_macos.c — macOS CSPRNG backend.
 *
 * Primary: SecRandomCopyBytes(kSecRandomDefault, len, buf) from
 *          Security.framework. Apple's /dev/random is non-blocking
 *          high-quality entropy on macOS (unlike Linux), so no fallback
 *          to /dev/urandom is needed.
 *
 * Linker flag: -framework Security (added in CMakeLists.txt on APPLE).
 *
 * Story 1-13 implementation.
 */

#if !defined(__APPLE__)
#error "t3_csprng_macos.c must only be compiled on Apple platforms"
#endif

#include <Security/Security.h>
#include <stdint.h>
#include <stddef.h>

#include "t3.h"
#include "t3_csprng.h"

t3_result_t t3_csprng_bytes(uint8_t *buf, size_t len) {
    if (len == 0) return T3_OK;
    /*
     * SecRandomCopyBytes fills exactly `len` bytes from kSecRandomDefault
     * (Apple's arc4random-backed CSPRNG seeded from kernel entropy).
     * Returns 0 (errSecSuccess) on success, non-zero on failure.
     *
     * The function is MT-Safe; Security.framework manages its own
     * internal state atomically.
     */
    int rc = SecRandomCopyBytes(kSecRandomDefault, len, buf);
    if (rc != errSecSuccess) {
        return T3_ERR_RNG;
    }
    return T3_OK;
}
