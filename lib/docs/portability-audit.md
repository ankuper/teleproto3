# libteleproto3 Portability Audit

**Scope:** `teleproto3/lib/src/*.c`, `teleproto3/lib/include/*.h`, `teleproto3/lib/include/internal/*.h`  
**Excluded:** `teleproto3/server/` (Linux-only by architectural intent — epoll/proc/signals; not in scope)  
**Date:** 2026-05-03 (Story 1-11)  
**Auditor:** dev-story execution

---

## 1. POSIX / glibc-only Headers

**Result: NONE found.**

All `#include` directives in `lib/src/*.c` and `lib/include/**/*.h` reference C standard library headers only:

| File | Headers included |
|------|-----------------|
| `src/t3_secret.c` | `<stdlib.h>`, `<string.h>`, `<stdint.h>` |
| `src/t3_session_header.c` | `<stdlib.h>`, `<string.h>`, `<stdint.h>` |
| `src/t3_timing.c` | `<stdint.h>`, `<limits.h>` |
| `src/t3_retry.c` | `<stdint.h>`, `<string.h>` |
| `src/t3_url_parser.c` | `<stdlib.h>`, `<string.h>`, `<stdint.h>` |
| `src/t3_strerror.c` | _(none beyond t3.h)_ |
| `src/t3_abi_version.c` | _(none beyond t3.h)_ |
| `src/t3_keyderiv.c` | _(stub; none beyond t3.h)_ |
| `src/t3_obfuscated2.c` | _(stub; none beyond t3.h)_ |
| `include/t3.h` | `<stddef.h>`, `<stdint.h>` |
| `include/internal/retry.h` | `<stdint.h>` |
| `include/internal/session.h` | `<stdint.h>`, `<stddef.h>` |
| `include/internal/timing.h` | `<stdint.h>` |

**Disposition:** No action required. All headers are C99/C11 standard and portable across Linux, macOS, Windows/MSVC.

The headers that AC #1 required scanning for — `<arpa/inet.h>`, `<sys/epoll.h>`, `<sys/time.h>`, `<unistd.h>`, `<endian.h>`, `<linux/*.h>`, `<sys/random.h>` — are **absent** from `lib/`.

---

## 2. Linux-only API Call Sites

**Result: NONE found.**

Patterns scanned: `epoll_`, `getrandom`, `/proc/`, `__builtin_`, `/dev/urandom`, `syscall`, `getentropy`.

| API pattern | Occurrences in `lib/` |
|-------------|----------------------|
| `epoll_*` | 0 |
| `getrandom` | 0 |
| `/proc/` reads | 0 |
| `__builtin_*` | 0 |
| `syscall(SYS_*)` | 0 |
| `getentropy` | 0 |
| `/dev/urandom` | 0 |

**Entropy / RNG architecture note.** The library uses zero platform-specific entropy calls. All randomness is consumed through `t3_callbacks_t.rng()` — a host-provided callback pointer. The consuming application (tdesktop, server, producer-tooling) supplies its own CSPRNG implementation. This design was intentional and makes `lib/` fully entropy-agnostic.

Story 1-11 adds `t3_csprng_bytes()` as a standardised interface for library-internal use (see §5 below), but the existing implementation already satisfies the portability requirement.

**Disposition:** No refactoring needed for call-site elimination. CSPRNG abstraction (Story 1-11 Task 4) is additive.

---

## 3. Byte-Order Conversion Sites

**Result: NONE found in `lib/`.**

Patterns scanned: `htonl`, `ntohl`, `htons`, `ntohs`, `be64toh`, `le64toh`, `htobe`, `htole`, `__BYTE_ORDER`.

`t3_header_t.flags` is documented as "host byte order in struct; little-endian on wire." The `t3_header_parse` / `t3_header_serialise` functions handle the conversion internally using direct byte-level access (shifts and masking), not the BSD/POSIX `hton*` / `be*toh` family.

**Disposition:** No action required. The existing approach is portable and works identically on all platforms including Windows/MSVC.

---

## 4. GNU Compiler Extension Sites

### 4a. `__attribute__((visibility("hidden")))` — 7 sites

Used on internal (non-`T3_API`) symbols to ensure they are not exported from a shared library build:

| File | Lines | Symbol |
|------|-------|--------|
| `include/internal/retry.h` | 24, 26, 28 | `t3_retry_ring_record`, `t3_retry_ring_get`, `t3_retry_ring_user_retry` |
| `include/internal/session.h` | 32 | `t3_session_init_internal` |
| `include/internal/timing.h` | 16 | `t3_timing_rejection_sample_uniform_ns` |
| `src/t3_retry.c` | 21 | `t3_retry_ring_record` (definition) |
| `src/t3_session_header.c` | 76 | `t3_session_init_internal` (definition) |
| `src/t3_timing.c` | 17 | `t3_timing_rejection_sample_uniform_ns` (definition) |
| `src/t3_url_parser.c` | 60 | `t3_url_parse_internal` (definition) |

**Disposition (CLOSED in 1-11 review pass, 2026-05-03):** `T3_HIDDEN` macro added at `lib/include/internal/t3_platform.h`:

```c
#if defined(__GNUC__) || defined(__clang__)
#  define T3_HIDDEN __attribute__((visibility("hidden")))
#else
#  define T3_HIDDEN
#endif
```

For GCC/Clang/AppleClang: expands to `__attribute__((visibility("hidden")))` — semantically unchanged.
For MSVC (static archive only, Story 1-11 scope): expands to nothing — internal symbols in a `.lib` are not exported regardless, and the GCC attribute syntax errors under `/W4 /WX`.

All 7 raw occurrences substituted with `T3_HIDDEN`; verified by `grep -rn "__attribute__\s*((visibility" lib/src lib/include` returning only the `T3_HIDDEN` macro definition itself and the `T3_API` macro definition in `t3.h`. ABI symbol-list snapshot unchanged (visibility is a link-time property; static archive `nm` output is identical).

### 4b. `__attribute__((format(printf, 3, 4)))` — 1 site

| File | Line | Usage |
|------|------|-------|
| `include/t3.h` | 157 | `t3_callbacks_t.log_sink` function pointer |

Already guarded by `#if defined(__GNUC__) || defined(__clang__)` with a bare declaration in the `#else` branch. **No action required** — correctly portable.

### 4c. `_Static_assert` — 4 sites in `t3.h`

All guarded by `#if (defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L) || defined(__cplusplus)`. MSVC 2019+ supports `_Static_assert` in C11 mode (`/std:c11`). **No action required.**

---

## 5. CSPRNG Abstraction (Story 1-11 deliverable)

Story 1-11 introduces `lib/include/internal/t3_csprng.h` declaring:

```c
t3_result_t t3_csprng_bytes(uint8_t *buf, size_t len);
```

This is an **additive** internal interface for future use by stub implementations in `t3_keyderiv.c` and `t3_obfuscated2.c` (currently stubs; implemented in Story 1-7 follow-on). Platform backends:

| File | Implementing story |
|------|--------------------|
| `lib/src/csprng/t3_csprng_linux.c` | Story 1-12 (`getrandom(2)` / `/dev/urandom`) |
| `lib/src/csprng/t3_csprng_macos.c` | Story 1-13 (`SecRandomCopyBytes` / `getentropy`) |
| `lib/src/csprng/t3_csprng_windows.c` | Story 1-14 (`BCryptGenRandom`) |

---

## 6. Per-Finding Disposition Summary

| Category | Count | Disposition |
|----------|-------|-------------|
| POSIX/glibc-only headers | 0 | No action |
| Linux-only API call sites | 0 | No action |
| Byte-order conversion sites | 0 | No action |
| `__attribute__((visibility("hidden")))` | 7 | **CLOSED 2026-05-03** — `T3_HIDDEN` macro added in `lib/include/internal/t3_platform.h`; all 7 sites substituted |
| `__attribute__((format(printf, ...)))` | 1 | Already conditionally guarded — no action |
| `_Static_assert` | 4 | Already conditionally guarded — no action |
| GNU statement expressions | 0 | No action |
| `__builtin_*` GCC extensions | 0 | No action |

**Overall finding:** `teleproto3/lib/` is fully portable as of Story 1-11 close-out. Zero POSIX-only headers, zero Linux-only API call sites, zero byte-order calls, zero raw `__attribute__((visibility("hidden")))` outside the `T3_HIDDEN` macro definition. Stories 1-12/1-13/1-14 add per-platform CSPRNG backends and any additional hardening flags; the visibility-macro abstraction does not need to be revisited.

---

## 7. References

- `lib/include/t3.h` — public API and `T3_API` macro
- `lib/include/internal/retry.h`, `session.h`, `timing.h` — internal visibility usage
- `lib/src/t3_timing.c` — sole `rng` callback consumer
- `lib/src/t3_session_header.c` — callback validation
- Story 1-6 ABI freeze — public symbol surface
- Story 1-12/1-13/1-14 — per-platform hardening and CSPRNG implementation
