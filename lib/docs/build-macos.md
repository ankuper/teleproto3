# Building libteleproto3 on macOS

macOS (Apple Silicon, arm64) with Xcode 14+ / AppleClang 14+ is the
**secondary reference platform** for libteleproto3. The macOS build is
the critical-path prerequisite for story 2.9 (macOS dev-build of
`Telegram.app` with Type3 proxy support).

See [`build.md`](build.md) for the platform-agnostic canonical build
command. This document covers macOS-specific prerequisites, the supported
toolchain, CSPRNG backend, architecture policy, and how to verify against
the Linux reference hashes.

---

## Prerequisites

```bash
xcode-select --install          # Xcode Command Line Tools (AppleClang 14+)
brew install cmake openssl@3    # CMake ≥ 3.20 + OpenSSL 3.x
```

- **Xcode Command Line Tools** ≥ 14 (AppleClang 14+, ships with Xcode 14).
  Check with `clang --version`. Required for `-Wall -Wextra -Wpedantic -Werror`.
- **cmake** ≥ 3.20 (`brew install cmake` gives 4.x; any ≥ 3.20 works).
- **openssl@3** from Homebrew. macOS ships LibreSSL at `/usr/bin/openssl`,
  but its ABI diverges from OpenSSL 3.x. Pin to Homebrew openssl@3 for
  consistency with Linux (story 1-12) and Windows (story 1-14).

---

## Standard Build

```bash
cmake -S teleproto3/lib -B build \
  -DCMAKE_BUILD_TYPE=Release \
  -DT3_CSPRNG_BACKEND=macos \
  -DCMAKE_OSX_DEPLOYMENT_TARGET=12.0 \
  -DOPENSSL_ROOT_DIR=$(brew --prefix openssl@3)

cmake --build build --config Release
```

This produces `build/libteleproto3.a` (arm64). Build uses
`-Wall -Wextra -Wpedantic -Werror -std=gnu11 -fvisibility=hidden`; a
compiler warning is a build failure.

---

## Deployment Target

`CMAKE_OSX_DEPLOYMENT_TARGET=12.0` (macOS 12 Monterey):

- Apple Silicon support begins at macOS 11 (Big Sur); macOS 12 is the
  safe minimum that covers all M1/M2/M3 systems likely to run producer
  tooling or run a dev-build of `Telegram.app`.
- macOS 11 and earlier are **out of scope** for v0.1.x.

---

## CSPRNG Backend

The macOS backend (`T3_CSPRNG_BACKEND=macos`) is implemented in
`src/csprng/t3_csprng_macos.c`:

- **API**: `SecRandomCopyBytes(kSecRandomDefault, len, buf)` from
  `Security.framework`.
- **Quality**: Apple's `/dev/random` is non-blocking and high-quality on
  macOS (unlike Linux where `/dev/random` blocks). No fallback to
  `/dev/urandom` is needed.
- **Error handling**: Non-zero return from `SecRandomCopyBytes` maps to
  `T3_ERR_RNG`; callers must treat this as fatal.

The linker flag `-framework Security` is added automatically by CMake
when `APPLE` is true (wired in `CMakeLists.txt`). No manual linker flag
is needed.

---

## Universal Binary Policy (arm64 + x86_64)

arm64 (Apple Silicon) is **mandatory**. x86_64 is **best-effort** and
gated on a CMake option (default OFF).

| Mode | CMake option | `lipo -info` result |
|------|-------------|---------------------|
| arm64 only (default) | `T3_MACOS_X86_64=OFF` | `architecture: arm64` |
| Universal (best-effort) | `T3_MACOS_X86_64=ON` | `arm64 x86_64` (if brew bottle supports x86_64) |

```bash
# Build universal binary (if brew openssl@3 has x86_64 bottle):
cmake -S teleproto3/lib -B build-universal \
  -DCMAKE_BUILD_TYPE=Release \
  -DT3_CSPRNG_BACKEND=macos \
  -DCMAKE_OSX_DEPLOYMENT_TARGET=12.0 \
  -DT3_MACOS_X86_64=ON \
  -DOPENSSL_ROOT_DIR=$(brew --prefix openssl@3)

cmake --build build-universal --config Release
lipo -info build-universal/libteleproto3.a
```

**Note:** On `macos-14` GitHub Actions runners, the Homebrew `openssl@3`
bottle is typically arm64-only. `libteleproto3.a` itself will be fat
(arm64+x86_64 object files compile without OpenSSL), but test executables
that link OpenSSL will fail for x86_64. The CI job degrades gracefully
and emits a `::warning::` annotation; the arm64-only mandatory build
remains green.

---

## Running Tests

After any build with `T3_BUILD_TESTS=ON` (the default):

```bash
ctest --test-dir <build-dir> --output-on-failure
```

Tests registered by the macOS build:

| Test name | Source | What it verifies |
|-----------|--------|--------------------|
| `csprng_macos` | `tests/csprng/test_csprng_macos.c` | 1 MiB chi-square ±5σ uniformity |
| `kdf_vectors_macos` | `tests/vectors/check_kdf_vectors.cmake` | KDF output matches `linux-reference.sha256` |

---

## Verification: Byte-Equivalence to Linux Reference

`lib/tests/vectors/linux-reference.sha256` is the **canonical KDF hash
file** generated on Linux (story 1-12, reference platform). macOS arm64
must produce byte-identical output per story 1-13 AC #5.

```bash
# Generate macOS output and verify against Linux reference:
./build/run_kdf_vectors \
  teleproto3/conformance/vectors/kdf-kat.txt \
  > /tmp/macos-arm64.sha256

grep -v '^#' /tmp/macos-arm64.sha256 > /tmp/macos-clean.sha256
grep -v '^#' teleproto3/lib/tests/vectors/linux-reference.sha256 > /tmp/linux-clean.sha256

diff /tmp/linux-clean.sha256 /tmp/macos-clean.sha256 && echo "PASS" || echo "FAIL: wire-format divergence"
```

A diff failure means the macOS port has a wire-format bug. Story 1-13
cannot close until this check passes.

---

## Downstream Consumers

**Story 2.9 (macOS dev-build) consumes `build/libteleproto3.a` produced
by this story; 2.9 dev MAY assume the artefact exists and the symbols are
exported per `tests/abi/symbol_list.txt`.**

This is the explicit handoff point — story 2.9 starts after story 1-13
status reaches `review` or `done`. Story 2.9 static-links
`libteleproto3.a` into `Telegram.app`. Without story 1-13 producing a
known-good arm64 archive that byte-matches the Linux reference, story 2.9
risks shipping wire-format-divergent binaries.

---

## Cross-References

- [`build.md`](build.md) — canonical CMake invocation, all platforms
- [`build-linux.md`](build-linux.md) — Linux reference build + KDF authority
- [`portability-audit.md`](portability-audit.md) — POSIX dependency audit (zero leaks confirmed)
- Story 1-11 — CMake skeleton + CSPRNG interface (`t3_csprng.h`)
- Story 1-12 — Linux reference build + `linux-reference.sha256` authority
- Story 1-14 — Windows build (also verifies against `linux-reference.sha256`)
- Story 2.9 — downstream consumer; this story is the gate
