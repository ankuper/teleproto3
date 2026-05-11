/*
 * t3_shim_socks5.h — localhost SOCKS5/CONNECT shim that tunnels through Type3 WSS.
 *
 * Accepts SOCKS5 connections on a localhost port. Per Story 9-1 D6, the shim
 * REQUIRES SOCKS5 USERNAME/PASSWORD authentication (RFC 1929, method 0x02) so
 * other local processes on the same machine cannot piggy-back through the
 * loopback listener. Credentials are auto-generated per spawn — retrieve them
 * via t3_shim_get_credentials() and pass them to the local client (tgcalls).
 * NO-AUTH (method 0x00) is no longer accepted.
 *
 * Tunnels each CONNECT through a new Type3 WSS session to the configured server.
 * CMD=BIND and CMD=UDP-ASSOCIATE return REP=0x07 (Command not supported).
 *
 * Build-flag-gated: compile only when T3_SHIM_SOCKS5=ON in CMake.
 * This header is included unconditionally; presence is checked via t3_features.h.
 *
 * // XXX 9-2: jitter randomization required before public-release widening
 * //          (CBR-tell verdict BLOCK 2026-05-09; see
 * //          _bmad-output/experiments/cbr-tell-2026-05-08/RESULT.md).
 *
 * API surface (AC #1, Story 9-1):
 *   t3_shim_open()              — open a new shim listener
 *   t3_shim_close()             — close the shim and all active tunnels
 *   t3_shim_local_port()        — query the bound localhost port
 *   t3_shim_get_credentials()   — retrieve auto-generated USER/PASS (D6)
 *   t3_shim_stats()             — active tunnel count + byte counters
 *
 * Stability: v0.1.2 additive. No existing symbol modified. Gated by
 * T3_SHIM_SOCKS5_AVAILABLE in t3_features.h.
 */

#ifndef T3_SHIM_SOCKS5_H
#define T3_SHIM_SOCKS5_H

#include "t3.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Opaque shim handle. */
typedef struct t3_shim t3_shim_t;

/*
 * t3_shim_open — create a SOCKS5 listener and start the background thread.
 *
 * @server_host   hostname of the Type3 server (NUL-terminated, ≤255 chars)
 * @server_port   TCP port of the Type3 server (typically 443)
 * @ws_path       WebSocket path, starting with '/' (NUL-terminated, ≤511 chars)
 * @secret_hex    full Type3 secret as lower-case hex string: "ff<32 hex key digits><domain>"
 * @local_port    hint for the localhost bind port; 0 = ephemeral (OS assigns)
 * @out           receives the newly created handle on T3_OK
 *
 * Returns T3_OK on success. Returns T3_ERR_INVALID_ARG if any pointer is NULL
 * or string limits are exceeded. Returns T3_ERR_INTERNAL if the listen socket
 * cannot be bound.
 */
T3_API t3_result_t t3_shim_open(
    const char  *server_host,
    uint16_t     server_port,
    const char  *ws_path,
    const char  *secret_hex,
    uint16_t     local_port,
    t3_shim_t  **out);

/*
 * t3_shim_close — stop the shim, close all tunnels, free all resources.
 * Safe to call with NULL (no-op).
 */
T3_API void t3_shim_close(t3_shim_t *shim);

/*
 * t3_shim_local_port — return the localhost port the shim is listening on.
 * Returns 0 if shim is NULL.
 */
T3_API uint16_t t3_shim_local_port(const t3_shim_t *shim);

/*
 * Length (in bytes, NOT counting the trailing NUL) of the auto-generated
 * USER and PASS strings produced by t3_shim_open(). They are lower-case
 * hex encodings of 16 random bytes each, so 32 hex digits == 32 bytes plus
 * a NUL terminator. Callers must size their buffers >= T3_SHIM_CRED_BUFLEN.
 */
#define T3_SHIM_CRED_LEN     32   /* hex chars, no NUL */
#define T3_SHIM_CRED_BUFLEN  33   /* includes trailing NUL */

/*
 * t3_shim_get_credentials — retrieve the auto-generated SOCKS5 USER/PASS
 * that this shim's loopback listener will require (D6, Story 9-1).
 *
 * @shim       opened shim handle (must not be NULL)
 * @out_user   buffer for the USER string; must be >= T3_SHIM_CRED_BUFLEN
 * @user_len   size of @out_user in bytes
 * @out_pass   buffer for the PASS string; must be >= T3_SHIM_CRED_BUFLEN
 * @pass_len   size of @out_pass in bytes
 *
 * On success the buffers are populated with NUL-terminated hex strings.
 * Returns T3_OK on success, T3_ERR_INVALID_ARG if any pointer is NULL or
 * the buffers are too small.
 *
 * The credentials remain stable for the lifetime of the shim handle; they
 * are not rotated. They MUST NOT be logged, persisted, or transmitted off
 * the local machine — their only purpose is to gate the loopback listener.
 */
T3_API t3_result_t t3_shim_get_credentials(
    const t3_shim_t *shim,
    char            *out_user, size_t user_len,
    char            *out_pass, size_t pass_len);

/*
 * t3_shim_stats — retrieve live counters.
 *
 * @out_active_tunnels  current number of open tunnels (gauge); may be NULL
 * @out_bytes_up        total bytes sent toward the Type3 server; may be NULL
 * @out_bytes_down      total bytes received from the Type3 server; may be NULL
 */
T3_API void t3_shim_stats(
    const t3_shim_t *shim,
    int32_t         *out_active_tunnels,
    uint64_t        *out_bytes_up,
    uint64_t        *out_bytes_down);

#ifdef __cplusplus
}
#endif

#endif /* T3_SHIM_SOCKS5_H */
