# lib/ Changelog

Notable changes to `libteleproto3`. Follows [SemVer](https://semver.org/)
with prefixed tags (`lib-v0.1.0`, `lib-v1.0.0`, ...). Each release
declares the `spec-vX.Y.Z` range it implements — see `VERSION`.

## [Unreleased]

### Added
- `t3_shim_get_credentials()` — retrieve the auto-generated SOCKS5
  USERNAME/PASSWORD that the shim enforces on its loopback listener
  (Story 9-1 D6). New constants `T3_SHIM_CRED_LEN` (32) and
  `T3_SHIM_CRED_BUFLEN` (33) declared in `t3_shim_socks5.h`.

## [lib-v0.7.0] — 2026-06-17

### Added
- CMake: `CMAKE_SYSTEM_NAME=iOS` now recognised — maps to `macos` CSPRNG
  backend (`SecRandomCopyBytes`) and links `Security.framework` automatically.
- CI: prebuilt `libteleproto3.a` for **iOS** (device arm64) and **iOS
  simulator** (fat arm64+x86_64) shipped in GitHub releases.
- CI: prebuilt `libteleproto3.a` for **Android** all four ABIs
  (`arm64-v8a`, `armeabi-v7a`, `x86`, `x86_64`) shipped in GitHub releases.
  Built with NDK r26d + inline OpenSSL 3.x cross-compile (no Telegram-Android
  BoringSSL dependency).
- `ci/build-ios.sh` — local cross-compile script for iOS device + simulator.
- `ci/build-android-release.sh` — CI-grade Android cross-compile script.

### Stability note
ABI unchanged — no existing function signature or enumerant changed.

## [lib-v0.6.0] — 2026-06-17

### Fixed
- Windows/MSVC: `winsock2.h` inclusion order fixed for socket type definitions
  (`SOCKET`/`INVALID_SOCKET` undefined errors).
- Windows/MSVC: portable `strndup`/`strdup` shims; silenced C4996 deprecation
  warnings on POSIX CRT functions (`strerror`, `strdup`).

### Added
- CI: prebuilt `libteleproto3.a` for Windows and macOS shipped in GitHub
  releases alongside headers and Linux archive.
- CI: release assets bundled with headers + lib + spec in source-tree layout
  for drop-in consumer integration.

### Stability note
ABI additive — no existing function signature or enumerant changed.
`T3_ABI_VERSION_MINOR` bumped `5 → 6`.

## [lib-v0.5.0] — 2026-06-09

### Fixed
- Ring buffers enlarged to 8 MiB; reassembly backpressure added. Previous
  256 KiB cap caused data loss on sync bursts with large MTProto containers.
- `t3_client_read` now returns exactly one complete MTProto message per call,
  matching the one-write-per-message invariant on the send path.
- `t3_client_*` data path reworked to match the server's framing expectations
  (Epic 10/12 regression introduced during tdlib integration).
- HTTP response headers are now correctly consumed when they arrive after
  the start of a chunked body (edge case in HTTP/1.1 pipelining).
- Android (API < 28): CSPRNG falls back to `/dev/urandom` instead of
  `getrandom()` syscall; fixes crash on older devices.
- Windows/MSVC: portable `strndup`/`strdup` shims; `winsock2.h` inclusion
  order fixed for socket type definitions.

### Added
- CI: Android NDK cross-compilation script and GitHub Actions workflow.
- CI: `build-lib` step produces prebuilt `libteleproto3.a` for Linux and
  uploads it to the GitHub release alongside headers.
- CI: `x86` ABI added to default Android build matrix (emulator support).

## [lib-v0.4.0] — 2026-05-30

### Changed
- Client-side transport build is now optional via CMake flag
  `T3_BUILD_CLIENT` (default `OFF`). When `OFF`, `t3_client_stream.c`
  is excluded from the archive — the static library has no OpenSSL/TLS
  symbols, which prevents the `nm-audit` symbol-leak failure on
  tdesktop's macOS/Linux builds and MSVC linker errors on Windows.
  Enable with `-DT3_BUILD_CLIENT=ON` when building a full client SDK.
- SOCKS5 shim loopback listener now **requires** RFC 1929 USERNAME/PASSWORD
  authentication (method `0x02`). NO-AUTH (`0x00`) is no longer accepted.
  Credentials are auto-generated per shim spawn; retrieve with
  `t3_shim_get_credentials()` (see `[Unreleased]`). Defense against local
  processes hijacking the loopback listener and tunneling via Type3.

### Stability note
`t3_shim_open` signature unchanged — additive change only.

## [lib-v0.3.0] — 2026-05-26

### Added
- **Client-side transport API** (`t3_client.h`): opaque handle
  `t3_client_stream` with state machine `CONNECTING → TLS → HANDSHAKE → READY`.
  Functions: `t3_client_create`, `t3_client_pump`, `t3_client_get_fd`,
  `t3_client_get_state`, `t3_client_write`, `t3_client_read`,
  `t3_client_destroy`, `t3_client_last_error`.
  Used by tdlib, tdesktop, Telegram-Android, Telegram-iOS. (Epics 10, 12;
  Story 12-5.)
- `t3_client_crypto.c`: obfs2 KDF + AES-256-CTR key derivation for client-side
  use. Shared by both WebSocket and HTTP stream transport paths.
- `t3_client_ws.c`: WebSocket frame write (masked per RFC 6455 §5.3) and read
  for client-side use (legacy transport path).
- `extern "C"` guards on `t3_client_crypto.h` and `t3_client_ws.h` for C++
  consumers (tdlib, tdesktop).

### Stability note
New public header `t3_client.h` — ABI additive. Existing `t3.h` surface
unchanged. `T3_ABI_VERSION_MINOR` bumped `2 → 3`.

## [lib-v0.2.0] — 2026-05-26

### Added
- **HTTP chunked stream framing** (`t3_http_stream.c`): HTTP/1.1
  `POST` + `Transfer-Encoding: chunked` for both request and response
  directions. Traffic is indistinguishable from a normal HTTPS REST call —
  resistant to WebSocket-specific DPI fingerprinting (ТСПУ). (Epic 12,
  Story 12-4.)
- `T3_TRANSPORT_WEBSOCKET` / `T3_TRANSPORT_HTTP_STREAM` transport mode enum
  added to `t3.h`. HTTP stream endpoint URLs use the `https://` scheme;
  WebSocket uses `wss://`. Secrets select the mode via `?t=1` query parameter
  (A-012, `spec/secret-format.md §2.4`).
- `T3_FLAG_PADDING` flag in the session header (bit 0): unilateral capability
  advertisement — client signals support for receiving server-injected padding
  frames. Anti-statistical-fingerprinting defence. (Epic 11, Story 11-1,
  spec amendment W-004.)
- Padding/splitting API (`t3_padding.c`): padding frame injection on the send
  path and receive-side discard. Padding frames are binary WebSocket / HTTP
  chunks with first decrypted byte `0xFE`; AES-CTR counter advances normally
  (Invariant 2).

### Stability note
`T3_ABI_VERSION_MINOR` bumped `1 → 2`.

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
