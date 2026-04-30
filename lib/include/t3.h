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

/*
 * Stability: Frozen for lib-v0.1.x. Adding a new function or a new
 * field to t3_callbacks_t (beyond the forward-compat struct_size sentinel)
 * requires lib-v0.2.0. Adding a new enumerant to t3_result_t is permitted
 * in any lib-v0.1.x patch and consumers MUST treat unknown values as
 * T3_ERR_INTERNAL. Little-endian byte order is normative for every
 * multi-byte field on the wire (cross-ref spec/wire-format.md §3).
 */

#ifndef TELEPROTO3_T3_H
#define TELEPROTO3_T3_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stddef.h>
#include <stdint.h>

/* T3_API — public-symbol export marker. */
#if defined(_WIN32) && !defined(__CYGWIN__)
#  if defined(T3_STATIC_LIB)
#    define T3_API
#  elif defined(T3_BUILDING_DLL)
#    define T3_API __declspec(dllexport)
#  else
#    define T3_API __declspec(dllimport)
#  endif
#elif defined(__GNUC__) || defined(__clang__) || defined(__CYGWIN__)
#  define T3_API __attribute__((visibility("default")))
#else
#  define T3_API
#endif

#define T3_LIB_VERSION_MAJOR 0
#define T3_LIB_VERSION_MINOR 1
#define T3_LIB_VERSION_PATCH 0

#define T3_ABI_VERSION_MAJOR 0
#define T3_ABI_VERSION_MINOR 1
#define T3_ABI_VERSION_PATCH 0

#if (defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L) || defined(__cplusplus)
#  if !defined(T3_LIB_VERSION_MAJOR) || !defined(T3_LIB_VERSION_MINOR) || !defined(T3_LIB_VERSION_PATCH)
#    error "T3_LIB_VERSION_* macros missing"
#  endif
#  if !defined(T3_ABI_VERSION_MAJOR) || !defined(T3_ABI_VERSION_MINOR) || !defined(T3_ABI_VERSION_PATCH)
#    error "T3_ABI_VERSION_* macros missing"
#  endif
_Static_assert(T3_LIB_VERSION_MAJOR == T3_ABI_VERSION_MAJOR, "lib and ABI MAJOR must match");
_Static_assert(T3_LIB_VERSION_MINOR == T3_ABI_VERSION_MINOR, "lib and ABI MINOR must match");
_Static_assert(T3_LIB_VERSION_PATCH == T3_ABI_VERSION_PATCH, "lib and ABI PATCH must match");
_Static_assert(T3_ABI_VERSION_MAJOR >= 0 && T3_ABI_VERSION_MINOR >= 0 && T3_ABI_VERSION_PATCH >= 0,
               "ABI version components must be non-negative");
#endif

#define T3_INTERNAL_STR_(x) #x
#define T3_INTERNAL_STR(x)  T3_INTERNAL_STR_(x)

/* --------------------------------------------------------------------
 * Result codes — X-macro source of truth (PR2 / D2 resolution)
 * -------------------------------------------------------------------- */
#define T3_RESULT_LIST(X)                                                                          \
    X(T3_OK,                              0, "ok")                                                 \
    X(T3_ERR_INVALID_ARG,                -1, "invalid argument")                                   \
    X(T3_ERR_MALFORMED,                  -2, "malformed input")                                    \
    X(T3_ERR_UNSUPPORTED_VERSION,        -3, "unsupported version")                                \
    X(T3_ERR_RNG,                        -4, "rng failure")                                        \
    X(T3_ERR_HOST_EMPTY,                 -5, "host field is empty")                                \
    X(T3_ERR_HOST_INVALID,               -6, "host field is invalid")                              \
    X(T3_ERR_KEY_INVALID,                -7, "key field is invalid")                               \
    X(T3_ERR_BUF_TOO_SMALL,              -8, "output buffer too small")                            \
    X(T3_ERR_HOST_NON_ASCII,            -10, "host contains non-ASCII characters (rejected at v0.1.0)") \
    X(T3_ERR_CLOCK_BACKWARDS,           -11, "monotonic clock went backwards")                     \
    X(T3_ERR_PATH_MISSING_LEADING_SLASH,-12, "path does not start with '/'")                       \
    X(T3_ERR_PATH_TRAILING_SLASH,       -13, "path has a trailing slash")                          \
    X(T3_ERR_PATH_PERCENT_ENCODED,      -14, "path contains percent-encoded octets")               \
    X(T3_ERR_PATH_EMPTY_SEGMENT,        -15, "path contains an empty segment ('//')")              \
    X(T3_ERR_PATH_NON_ASCII,            -16, "path contains non-ASCII characters (rejected at v0.1.0)") \
    X(T3_ERR_INTERNAL,                  -99, "internal error")

#define T3_RESULT_ENUM_ENTRY(name, value, msg) name = value,
typedef enum {
    T3_RESULT_LIST(T3_RESULT_ENUM_ENTRY)
    T3_RESULT_T_FORCE_INT_STORAGE = 0x7fffffff
} t3_result_t;
#undef T3_RESULT_ENUM_ENTRY

/* --------------------------------------------------------------------
 * Version-negotiation action (spec/wire-format.md §6)
 * -------------------------------------------------------------------- */
typedef enum {
    T3_VERSION_OK              = 0,
    T3_VERSION_SILENT_CLOSE    = 1,
    T3_VERSION_RETRY_DOWNGRADE = 2
} t3_version_action_t;

/* --------------------------------------------------------------------
 * Anti-probe retry state (spec/anti-probe.md §7; FR43)
 * -------------------------------------------------------------------- */
typedef enum {
    T3_RETRY_OK    = 0,
    T3_RETRY_TIER1 = 1,
    T3_RETRY_TIER2 = 2,
    T3_RETRY_TIER3 = 3
} t3_retry_state_t;

/* --------------------------------------------------------------------
 * Opaque handle types
 * -------------------------------------------------------------------- */
typedef struct t3_secret  t3_secret_t;
typedef struct t3_session t3_session_t;

/* --------------------------------------------------------------------
 * Session Header POD (spec/wire-format.md §3; AR-S1)
 *
 * flags is stored in host byte order in this struct. On the wire it is
 * little-endian. t3_header_parse converts LE→host; t3_header_serialise
 * converts host→LE. Cross-ref spec/wire-format.md §3.
 * -------------------------------------------------------------------- */
typedef struct {
    uint8_t  command_type;
    uint8_t  version;
    uint16_t flags;   /* host byte order in struct; little-endian on wire */
} t3_header_t;

#if (defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L) || defined(__cplusplus)
_Static_assert(sizeof(t3_header_t) == 4,
               "t3_header_t must be exactly 4 bytes (no padding) — wire shape is frozen at v0.1.0");
#endif

/* --------------------------------------------------------------------
 * Host-stack callbacks (epic-1-style-guide §10; AR-S12)
 * -------------------------------------------------------------------- */
typedef struct {
    size_t   struct_size;
    int64_t  (*lower_send)(void *ctx, const uint8_t *buf, size_t len);
    int64_t  (*lower_recv)(void *ctx, uint8_t *buf, size_t len);
    int64_t  (*frame_send)(void *ctx, const uint8_t *buf, size_t len, int is_binary);
    int64_t  (*frame_recv)(void *ctx, uint8_t *buf, size_t cap, int *out_is_binary);
    int      (*rng)(void *ctx, uint8_t *buf, size_t len);
    uint64_t (*monotonic_ns)(void *ctx);
#if defined(__GNUC__) || defined(__clang__)
    void     (*log_sink)(void *ctx, int level, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
#else
    void     (*log_sink)(void *ctx, int level, const char *fmt, ...);
#endif
    void    *ctx;
} t3_callbacks_t;

#if (defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L) || defined(__cplusplus)
_Static_assert(offsetof(t3_callbacks_t, struct_size) == 0,
               "t3_callbacks_t.struct_size must be the first field for forward-compat sentinel");
#endif

/* --------------------------------------------------------------------
 * Producer-side input struct (AC #15; spec/secret-format.md §3)
 * -------------------------------------------------------------------- */
typedef struct {
    uint8_t     key[16];
    const char *host;
    const char *path;
} t3_secret_fields;

/* ====================================================================
 * Secret parsing / serialisation  (spec/secret-format.md §1–4)
 * ==================================================================== */
T3_API t3_result_t t3_secret_parse(const uint8_t *buf, size_t len, t3_secret_t **out);
T3_API void        t3_secret_free(t3_secret_t *s);
T3_API t3_result_t t3_secret_serialise(const t3_secret_fields *in, uint8_t *out, size_t *inout_len);
T3_API t3_result_t t3_secret_validate_host(const char *host);
T3_API t3_result_t t3_secret_validate_path(const char *path);
T3_API void        t3_secret_zeroise(t3_secret_fields *fields);

/* ====================================================================
 * Session management  (spec/wire-format.md §1)
 * ==================================================================== */
T3_API t3_result_t t3_session_new(const t3_secret_t *s, t3_session_t **out);
T3_API void        t3_session_free(t3_session_t *sess);
T3_API t3_result_t t3_session_bind_callbacks(t3_session_t *sess, const t3_callbacks_t *cb);

/* ====================================================================
 * Session Header encode/decode  (spec/wire-format.md §3; AR-S1)
 * ==================================================================== */
T3_API t3_result_t t3_header_parse(const uint8_t buf[4], t3_header_t *out);
T3_API t3_result_t t3_header_serialise(const t3_header_t *in, uint8_t buf[4]);

/* ====================================================================
 * Version negotiation  (spec/wire-format.md §6)
 * ==================================================================== */
T3_API t3_result_t t3_session_negotiate_version(t3_session_t *sess,
                                                uint8_t peer_version,
                                                t3_version_action_t *out);

/* ====================================================================
 * Anti-probe: silent-close delay  (spec/anti-probe.md §8; AR-C2)
 * ==================================================================== */
T3_API t3_result_t t3_silent_close_delay_sample_ns(t3_session_t *sess, uint64_t *out_ns);

/* ====================================================================
 * Anti-probe: retry state machine  (spec/anti-probe.md §7; FR43)
 * ==================================================================== */
T3_API t3_result_t      t3_retry_record_close(t3_session_t *sess,
                                              uint64_t now_monotonic_ns,
                                              t3_retry_state_t *out_state);
T3_API t3_retry_state_t t3_retry_get_state(const t3_session_t *sess);
T3_API t3_result_t      t3_retry_user_retry(t3_session_t *sess);

/* ====================================================================
 * Utility
 * ==================================================================== */
T3_API const char *t3_strerror(t3_result_t rc);
T3_API const char *t3_abi_version_string(void);

#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif /* TELEPROTO3_T3_H */
