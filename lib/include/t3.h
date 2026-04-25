/*
 * t3.h — libteleproto3 public API.
 *
 * This is the reference implementation of the Type3 protocol.
 * Normative behaviour is defined in spec/. Where they differ, spec/ wins.
 * File a bug or errata at https://github.com/ankuper/teleproto3/issues.
 *
 * Stability: this header is the ONLY stable surface of the library.
 * External consumers (server/, tdesktop fork, iOS fork, Android fork)
 * MUST depend on this header alone. Everything under lib/src/ is
 * private and subject to change without notice.
 */

#ifndef TELEPROTO3_T3_H
#define TELEPROTO3_T3_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stddef.h>
#include <stdint.h>

/* Semantic version of the library this header ships with. Mirrors
 * lib/VERSION. */
#define T3_LIB_VERSION_MAJOR 0
#define T3_LIB_VERSION_MINOR 1
#define T3_LIB_VERSION_PATCH 0
#define T3_LIB_VERSION_PRERELEASE "draft"

/* Return codes returned by every public API entry point.
 * TODO(lib-v0.1.0): finalise enumerants alongside wire-format.md. */
typedef enum {
    T3_OK = 0,
    T3_ERR_INVALID_ARG = -1,
    T3_ERR_MALFORMED = -2,
    T3_ERR_UNSUPPORTED_VERSION = -3,
    T3_ERR_INTERNAL = -99
} t3_result_t;

/* Opaque handle types. */
typedef struct t3_secret t3_secret_t;
typedef struct t3_session t3_session_t;

/* --------------------------------------------------------------------
 * Secret parsing / serialisation (spec/secret-format.md)
 * -------------------------------------------------------------------- */

t3_result_t t3_secret_parse(const uint8_t *buf, size_t len, t3_secret_t **out);
void        t3_secret_free(t3_secret_t *s);

/* --------------------------------------------------------------------
 * Session handshake (spec/wire-format.md §1–3)
 * -------------------------------------------------------------------- */

t3_result_t t3_session_new(const t3_secret_t *s, t3_session_t **out);
void        t3_session_free(t3_session_t *sess);

/* --------------------------------------------------------------------
 * Framing (spec/wire-format.md §2, §4)
 * -------------------------------------------------------------------- */

/* TODO(lib-v0.1.0): decide whether framing is exposed as streaming
 * callbacks or a scatter-gather buffer interface. Match server/'s
 * rwm_* conventions where possible. */

#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif /* TELEPROTO3_T3_H */
