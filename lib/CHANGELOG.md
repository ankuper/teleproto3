# lib/ Changelog

Notable changes to `libteleproto3`. Follows [SemVer](https://semver.org/)
with prefixed tags (`lib-v0.1.0`, `lib-v1.0.0`, ...). Each release
declares the `spec-vX.Y.Z` range it implements — see `VERSION`.

## [Unreleased]

<!-- Future lib-v0.1.x patch entries go here. -->

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
