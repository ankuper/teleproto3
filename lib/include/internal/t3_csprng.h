/*
 * t3_csprng.h — internal CSPRNG abstraction interface.
 *
 * Platform implementations live in lib/src/csprng/:
 *   t3_csprng_linux.c   — Story 1-12 (getrandom / /dev/urandom fallback)
 *   t3_csprng_macos.c   — Story 1-13 (SecRandomCopyBytes / getentropy)
 *   t3_csprng_windows.c — Story 1-14 (BCryptGenRandom)
 *
 * CMake selects the backend via T3_CSPRNG_BACKEND (AUTO by default).
 */

#ifndef TELEPROTO3_INTERNAL_T3_CSPRNG_H
#define TELEPROTO3_INTERNAL_T3_CSPRNG_H

#include <stdint.h>
#include <stddef.h>
#include "t3.h"

/*
 * t3_csprng_bytes — fill `buf` with `len` cryptographically random bytes.
 *
 * Thread-safety: MT-Safe. Callers on any thread may invoke concurrently;
 * platform backends must not rely on shared mutable state without locking
 * (most OS CSPRNG APIs are already MT-Safe).
 *
 * Returns: T3_OK on success, T3_ERR_RNG if the host RNG fails.
 *
 * Callers must treat T3_ERR_RNG as a fatal error — there is no meaningful
 * recovery path if the OS CSPRNG is unavailable.
 */
t3_result_t t3_csprng_bytes(uint8_t *buf, size_t len);

#endif /* TELEPROTO3_INTERNAL_T3_CSPRNG_H */
