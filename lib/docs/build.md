# libteleproto3 Build Guide

Introduced in Story 1-11. Supersedes the legacy `lib/build/Makefile` (retained one release for downstream consumers mid-migration; see §4).

---

## 1. Canonical Build Command

```bash
cmake -S teleproto3/lib -B teleproto3/lib/build -DCMAKE_BUILD_TYPE=Release
cmake --build teleproto3/lib/build
```

Output: `teleproto3/lib/build/libteleproto3.a`

**T3_CSPRNG_BACKEND options** (current default: `none`, per A-009):

| Value | Entropy source | Implemented by |
|-------|---------------|----------------|
| `none` | Returns `T3_ERR_RNG` always (current default) | Story 1-11 (portability gate) |
| `linux` | `getrandom(2)` / `/dev/urandom` | Story 1-12 |
| `macos` | `SecRandomCopyBytes` (Security.framework) | Story 1-13 |
| `windows` | `BCryptGenRandom` (bcrypt.lib) | Story 1-14 |
| `AUTO` | Host-detected from `CMAKE_SYSTEM_NAME` | Will become default after 1-14 closes |

The default is `none` until all three platform backends ship (per amendment A-009). This keeps `cmake -S … -B build` green for any developer running on a fresh clone before 1-12/1-13/1-14 land. After 1-14 closes, the CMake default flips to `AUTO` and host-detection picks the correct backend automatically.

**OpenSSL:** CMake finds OpenSSL ≥3.0 automatically. On macOS (Homebrew), set `OPENSSL_ROOT_DIR`:

```bash
cmake -S teleproto3/lib -B teleproto3/lib/build \
    -DOPENSSL_ROOT_DIR=$(brew --prefix openssl@3)
```

---

## 2. Cross-references

- **Portability audit** — [`portability-audit.md`](portability-audit.md) details every POSIX/glibc dependency and disposition.
- **Story 1-12** (Linux hardening) — implements `t3_csprng_linux.c`, adds ASan/UBSan CI cells.
- **Story 1-13** (macOS build) — implements `t3_csprng_macos.c`, enables macOS CI job; **unblocks Story 2-9** (macOS dev-build).
- **Story 1-14** (Windows build) — implements `t3_csprng_windows.c`, enables Windows CI job; precondition for Story 2-8.
- **Story 2-9** (macOS dev-build, consumer) — links against `libteleproto3.a` built by Story 1-13.
- **Story 2-8** (Windows release, consumer) — links against `libteleproto3.a` built by Story 1-14.

---

## 3. ABI Symbol Guard

A symbol-list baseline is committed at `tests/abi/symbol_list.txt`. After building, verify the ABI surface is unchanged:

```bash
bash teleproto3/lib/tests/abi/symbol_list_check.sh \
    teleproto3/lib/build/libteleproto3.a \
    teleproto3/lib/tests/abi/symbol_list.txt
```

The CI `linux` job runs this check automatically (see `.github/workflows/lib-portability.yml`).

To regenerate the baseline after an intentional ABI change:

```bash
# Run on Linux (canonical platform for ELF nm output)
nm --extern-only --defined-only teleproto3/lib/build/libteleproto3.a \
    | awk '$2 ~ /^[TDRBSWV]$/ {print $3}' \
    | LC_ALL=C sort -u > teleproto3/lib/tests/abi/symbol_list.txt
# On macOS, also pipe through 'sed s/^_//' before sort to strip Mach-O underscore.
# Review and commit
```

The committed snapshot has 25 entries: 18 public `T3_API` symbols (Story 1-6 frozen ABI surface) plus 7 hidden-visibility internal helpers that `nm` lists from a static archive regardless of visibility (visibility is a shared-library concept; static-archive `nm` shows all extern-defined symbols). The guard fails on any drift in either category.

---

## 4. Legacy Build Files (deprecated)

`lib/build/Makefile` and `lib/build/BUILD.bazel` carry a deprecation banner (added 2026-05-03). They are kept for one release as a courtesy to downstream consumers still using Make or Bazel. They are NOT executed in CI.

The **server** Makefile (`teleproto3/server/Makefile`) is unchanged and continues to govern the Linux-only proxy binary. CMake governs `lib/` only.

---

## 5. Per-Platform Follow-ups

| Story | Platform | Adds |
|-------|----------|------|
| 1-12 | Linux | `t3_csprng_linux.c`, ASan/UBSan hardened CI cells, `linux-reference.sha256` KDF golden vectors |
| 1-13 | macOS | `t3_csprng_macos.c`, arm64 + x86_64 (opt-in), brew openssl@3, macOS CI job enabled |
| 1-14 | Windows | `t3_csprng_windows.c`, MSVC 2022, vcpkg openssl x64-windows-static, Windows CI job enabled |

Platform stories are independent and may proceed in parallel once Story 1-11 merges.
