# lib/ Changelog

Notable changes to `libteleproto3`. Follows [SemVer](https://semver.org/)
with prefixed tags (`lib-v0.1.0`, `lib-v1.0.0`, ...). Each release
declares the `spec-vX.Y.Z` range it implements — see `VERSION`.

## [Unreleased]

### Added
- `t3_shim_get_credentials()` — retrieve the auto-generated SOCKS5
  USERNAME/PASSWORD that the shim now enforces on its loopback listener
  (Story 9-1 D6). New constants `T3_SHIM_CRED_LEN` (32) and
  `T3_SHIM_CRED_BUFLEN` (33) declared in `t3_shim_socks5.h`.

### Changed
- The shim's loopback SOCKS5 listener REQUIRES RFC 1929 USERNAME/PASSWORD
  authentication (method 0x02). NO-AUTH (method 0x00) is no longer accepted.
  Credentials are auto-generated per shim spawn; callers retrieve them via
  `t3_shim_get_credentials()`. This is defense against other local processes
  hijacking the loopback listener and tunneling traffic through Type3.

### Stability note
Additive C API only — `t3_shim_open` signature unchanged. ABI patch bump
will be applied at the next release tag.

## [lib-v0.4.0] — 2026-05-27

### Added
- **Client-side transport API** (`t3_client.h`): `t3_client_connect`,
  `t3_client_wrap`, `t3_client_unwrap`, `t3_client_close` — abstraction for
  establishing Type3 connections from client code. Supports both WebSocket
  and HTTP stream transport modes. Used by the tdlib Type3 integration.
  (Epic 13, Stories 13-1 through 13-4.)
- **HTTP stream framing** (`t3_http_stream.c`): HTTP chunked transfer
  encoding for POST-based transport mode. Implements `Transfer-Encoding:
  chunked` framing for both request and response directions. ТСПУ-resistant
  — traffic looks like a normal HTTPS POST to a REST API.
  (Epic 12, Story 12-2.)
- **Client WebSocket framing** (`t3_client_ws.c`): WebSocket frame write
  (masked per RFC 6455 §5.3) and read for client-side use.
- **Client crypto** (`t3_client_crypto.c`): obfs2 KDF + AES-256-CTR key
  derivation ported from server to client. Shared by both transport modes.
- `extern "C"` guards on internal headers (`t3_client_crypto.h`,
  `t3_client_ws.h`) for C++ consumers (tdlib).
- Padding/transport mode enums in `t3.h` (`T3_TRANSPORT_WEBSOCKET`,
  `T3_TRANSPORT_HTTP_STREAM`).

### Stability note
New public header `t3_client.h` — ABI additive. Existing `t3.h` surface
unchanged. `T3_ABI_VERSION_MINOR` bumped `1 → 4`.

### Changed
- The shim's loopback SOCKS5 listener REQUIRES RFC 1929 USERNAME/PASSWORD
  authentication (method 0x02). NO-AUTH (method 0x00) is no longer accepted.
  Credentials are auto-generated per shim spawn; callers retrieve them via
  `t3_shim_get_credentials()`. This is defense against other local processes
  hijacking the loopback listener and tunneling traffic through Type3.

### Stability note
Additive C API only — `t3_shim_open` signature unchanged. ABI patch bump
will be applied at the next release tag.

## [lib-v0.1.3] — 2026-05-12

### Added
- `T3_ERR_DOMAIN_TOO_LONG = -17` and `T3_ERR_INVALID_CONFIG = -18` added to
  `t3_result_t` X-macro list in `t3.h`. Story 7-1 introduces these as
  **internal detail-codes**, NOT new wire-error classes. Wire schema remains
  the three-class set (`MALFORMED | INVALID_ARG | UNSUPPORTED_VERSION`).
  Conformance vectors surface the detail via `expect.detail.lib_code`
  (additive, non-normative implementation hint). Lib→wire mapping:
  `T3_ERR_DOMAIN_TOO_LONG → MALFORMED`,
  `T3_ERR_INVALID_CONFIG → INVALID_ARG`.
  Additive — no existing value renumbered; consumers MUST treat unknown
  negative values as `T3_ERR_INTERNAL` per ABI stability contract.

### Stability note
ABI additive patch — no existing enumerant value or function signature changed.
`T3_ABI_VERSION_PATCH` bumped `2 → 3`.

## [lib-v0.1.2] — 2026-05-10

### Added
- Optional SOCKS5/CONNECT shim (`t3_shim_*` API). Build-flag-gated by
  `T3_SHIM_SOCKS5=ON` in CMake. Used by Epic 9 calls integration (Story 9-1).
  Public surface: `t3_shim_open()`, `t3_shim_close()`, `t3_shim_local_port()`,
  `t3_shim_stats()` — declared in `lib/include/t3_shim_socks5.h`.
  Consumers guard calls with `T3_SHIM_SOCKS5_AVAILABLE` from `t3_features.h`.
- `lib/include/t3_features.h` — compile-time feature availability flags header.

### Stability note
ABI additive patch — no existing function, field layout, or enumerant changed.
`T3_ABI_VERSION_PATCH` bumped `1 → 2`. Consumer pins updated:
`tdesktop/.../teleproto3_bridge.cpp` and `tdesktop/.../ui/proxy_indicator_c1.cpp`.

## [lib-v0.1.1] — 2026-05-06

### Added
- `command_type=0x04` (`T3_CMD_BENCH`) — experimental dev-only command for in-channel bench echo.
  Server dispatch must be build-flag-gated; see Epic 1a. (Stories 1a-1 through 1a-3.)
- Named `#define` constants `T3_CMD_MTPROTO_PASSTHROUGH`, `T3_CMD_HTTP_DECOY_MIMIC`,
  `T3_CMD_BENCH` in `t3.h` — documents the full `command_type` registry in one place.

### Changed
- `t3_header_parse()` now accepts `command_type=0x04` at `version=0x01` and returns `T3_OK`.
  Previously all values other than `0x01` were `MALFORMED` at known version. The change is
  additive: `0x01` behaviour and all rejection paths are unchanged.

### Stability note
ABI additive patch — no existing function signature, field layout, or enumerant value is
modified. `T3_ABI_VERSION_PATCH` bumped `0 → 1`; consumers that pinned to `0.1.0` MUST
update their `_Static_assert` to `0.1.1` (one-line edit, no ABI incompatibility).

## [lib-v0.1.0] — 2026-04-24

### Added
- Public ABI surface frozen for `lib-v0.1.x` per epic-1-style-guide §9:
  enums `t3_result_t` / `t3_version_action_t` / `t3_retry_state_t`;
  POD `t3_header_t` + `t3_callbacks_t`; functions `t3_header_parse`,
  `t3_header_serialise`, `t3_session_bind_callbacks`,
  `t3_session_negotiate_version`, `t3_silent_close_delay_sample_ns`,
  `t3_retry_record_close`, `t3_retry_get_state`, `t3_retry_user_retry`,
  `t3_strerror`, `t3_abi_version_string`; macros `T3_ABI_VERSION_*`.
- Producer-side ABI surface per AC #15: `t3_secret_serialise`,
  `t3_secret_validate_host`, `t3_secret_validate_path`; POD
  `t3_secret_fields`; fine-grained error enumerants for operator-UX
  (`T3_ERR_HOST_EMPTY`, `T3_ERR_HOST_INVALID`, `T3_ERR_PATH_INVALID`,
  `T3_ERR_KEY_INVALID`, `T3_ERR_BUF_TOO_SMALL`).
- `T3_ERR_RNG` enumerant in `t3_result_t` (AC #14 erratum — canonical
  form of `t3_silent_close_delay_sample_ns` returns `t3_result_t`, not
  `void`, so RNG failure is surfaced to the caller).
- `T3_API` export macro; `-fvisibility=hidden` in `lib/build/Makefile`
  CFLAGS and `lib/build/BUILD.bazel` `copts`.
- `_Static_assert` triple binding `T3_LIB_VERSION_*` to
  `T3_ABI_VERSION_*` so the two macro families cannot silently drift.
- Per-handle thread-safety contract documented in `t3.h` doc-comments.
- Little-endian wire serialisation of `t3_header_t::flags` documented
  normatively in `t3_header_serialise` / `t3_header_parse` doc-comments
  (cross-ref `spec/wire-format.md §3`).
- `lib/include/internal/` placeholder directory for Story 1.7 private
  headers.
- Scaffold: public header `include/t3.h`, source stubs under `src/`,
  POSIX Make + Bazel build files, unit/integration test placeholders.

### Changed
- `lib/VERSION` bumped from `0.1.0-draft` to `0.1.0`.
- `implements_spec` constraint from `">=0.1.0-draft, <0.2.0"` to
  `">=0.1.0, <0.2.0"`.
- Removed `T3_LIB_VERSION_PRERELEASE` macro (no prerelease at the
  published `0.1.0` tag; reintroduced only if a `lib-v0.2.0-rc1` ships).

### Build / CI
- `consumer-dispatch.yml` wired to fire `repository_dispatch` only on
  MAJOR or MINOR bumps; PATCH-only tags suppress dispatch.
- `banner-discipline.yml` extended to cover `lib/include/*.h`.

### Stability note
ABI Frozen for `lib-v0.1.x`. Adding a new function or a new field to
`t3_callbacks_t` requires `lib-v0.2.0`. Adding a new enumerant to
`t3_result_t` is permitted in any patch and consumers MUST treat
unknown values as `T3_ERR_INTERNAL`.

## [lib-v0.1.0-draft] — 2026-04-24

Draft initialisation. Not a published tag. Implements
`spec-v0.1.0-draft`; no runtime behaviour yet.
