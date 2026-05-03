# Building libteleproto3 on Linux

Linux (Ubuntu 22.04 / Debian 12, glibc, x86_64) is the **reference platform**
for libteleproto3. The KDF output computed on Linux is the canonical authority
used to verify the macOS and Windows ports (stories 1-13 / 1-14).

See [`build.md`](build.md) for the platform-agnostic canonical build command.
This document covers Linux-specific prerequisites, the supported compiler
matrix, sanitizer builds, hardened builds, and how to use the reference
hash file for cross-platform verification.

---

## Prerequisites

Ubuntu 22.04 / Debian 12:

```bash
sudo apt-get update
sudo apt-get install -y build-essential cmake libssl-dev pkg-config clang
```

- **cmake** ≥ 3.20
- **OpenSSL** ≥ 3.0 (`libssl-dev`)
- **gcc** ≥ 11 _or_ **clang** ≥ 14 (both required for the CI matrix)
- **cppcheck** (optional locally; required in CI)

---

## Supported Toolchain Matrix

| OS | Compiler | Build type | Status |
|----|----------|------------|--------|
| Ubuntu 22.04 | GCC 11+ | Release | ✅ Reference |
| Ubuntu 22.04 | Clang 14+ | Release | ✅ Required green |
| Ubuntu 22.04 | Clang 14+ | Debug + ASan/UBSan | ✅ Required green |
| Ubuntu 22.04 | GCC 11+ | Release + Hardened | ✅ Required green |
| Debian 12 | GCC 11+ | Release | ✅ Supported |

Alpine/musl and 32-bit Linux are **out of scope** for v0.1.x.

---

## Standard Build

```bash
cmake -S teleproto3/lib -B teleproto3/lib/build \
  -DCMAKE_BUILD_TYPE=Release \
  -DT3_CSPRNG_BACKEND=linux

cmake --build teleproto3/lib/build --config Release
```

To select the compiler explicitly:

```bash
cmake -S teleproto3/lib -B teleproto3/lib/build \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_C_COMPILER=clang \
  -DT3_CSPRNG_BACKEND=linux
```

Both compilers are built with `-Wall -Wextra -Wpedantic -Werror`; a warning is
a build failure.

---

## CSPRNG Backend

The Linux backend (`T3_CSPRNG_BACKEND=linux`) is implemented in
`src/csprng/t3_csprng_linux.c`:

- **Primary**: `getrandom(2)` with `GRND_NONBLOCK` (glibc ≥ 2.25, kernel ≥ 3.17).
- **Fallback**: `/dev/urandom` read loop on `ENOSYS` (older kernels) or `EAGAIN`
  (entropy pool not yet seeded at early boot).

The backend is MT-Safe. Callers must treat `T3_ERR_RNG` as fatal.

---

## Sanitizer Build

```bash
cmake -S teleproto3/lib -B teleproto3/lib/build-san \
  -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_C_COMPILER=clang \
  -DT3_CSPRNG_BACKEND=linux \
  -DT3_SANITIZE=address,undefined

cmake --build teleproto3/lib/build-san
ctest --test-dir teleproto3/lib/build-san --output-on-failure
```

`T3_SANITIZE` accepts a comma-separated list of sanitizers passed to
`-fsanitize=`. Applied to both the library and all test executables.
Clang is preferred for ASan/UBSan because it produces better stack traces.

**TSan is not enabled** — the library is single-threaded per ABI contract
(`t3_session_t` is owned by the caller; not thread-safe per story 1-6 AC #11).

---

## Hardened Build

```bash
cmake -S teleproto3/lib -B teleproto3/lib/build-hard \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_C_COMPILER=gcc \
  -DT3_CSPRNG_BACKEND=linux \
  -DT3_HARDENING=ON

cmake --build teleproto3/lib/build-hard --config Release
```

`T3_HARDENING=ON` adds:

| Flag | Rationale |
|------|-----------|
| `-fstack-protector-strong` | Stack canaries on functions with arrays/local pointers |
| `-D_FORTIFY_SOURCE=2` | glibc-side bounds checking on string/memory functions |
| `-fno-strict-aliasing` | Protocol code casts pointers for byte-order; conservative alias analysis |
| `-fno-omit-frame-pointer` | Useful stack traces under ASan and perf |

---

## Running Tests

After any build with `T3_BUILD_TESTS=ON` (the default):

```bash
ctest --test-dir <build-dir> --output-on-failure
```

Tests registered by the Linux build:

| Test name | Source | What it verifies |
|-----------|--------|-----------------|
| `csprng_linux` | `tests/csprng/test_csprng_linux.c` | 1 MiB chi-square ±5σ uniformity |
| `kdf_vectors_linux` | `tests/vectors/check_kdf_vectors.cmake` | KDF output matches `linux-reference.sha256` |

---

## Reference Vectors and Cross-Platform Verification

`lib/tests/vectors/linux-reference.sha256` is the **authoritative hash file**
generated from the KDF KAT inputs in
`teleproto3/conformance/vectors/kdf-kat.txt`:

```
kdf-001  2ec3b0cd2084693c...  (SHA256 of read_key||read_iv||write_key||write_iv)
…
```

Stories 1-13 (macOS) and 1-14 (Windows) **must** produce byte-identical KDF
output against these five vectors. Their AC #5 / AC #6 verify this by running
`run_kdf_vectors` with the same `kdf-kat.txt` and diffing against
`linux-reference.sha256`.

To regenerate the reference hash (only needed after a lib-v0.2.x KDF change):

```bash
cmake -S teleproto3/lib -B teleproto3/lib/build -DT3_CSPRNG_BACKEND=linux
cmake --build teleproto3/lib/build
./teleproto3/lib/build/run_kdf_vectors \
  teleproto3/conformance/vectors/kdf-kat.txt \
  > teleproto3/lib/tests/vectors/linux-reference.sha256
```

Commit the updated file alongside the ABI version bump.

---

## cppcheck (Static Analysis)

```bash
cppcheck \
  --enable=warning,style,performance,portability \
  --error-exitcode=1 \
  --suppressions-list=teleproto3/lib/.cppcheck-suppressions \
  teleproto3/lib/src \
  teleproto3/lib/include
```

The lib-specific suppressions file (`lib/.cppcheck-suppressions`) is separate
from `teleproto3/server/.cppcheck-suppressions` and must not be merged.
Each suppression entry requires an inline justification comment.

---

## Cross-References

- [`build.md`](build.md) — canonical CMake invocation, all platforms
- [`portability-audit.md`](portability-audit.md) — POSIX dependency audit
- Story 1-13 — macOS build (verifies against `linux-reference.sha256`)
- Story 1-14 — Windows build (verifies against `linux-reference.sha256`)
