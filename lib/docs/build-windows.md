# Building libteleproto3 on Windows

Windows x64 with MSVC 2022 is the **Windows reference platform** for
libteleproto3.  The Windows build is a prerequisite for story 2.8 (Windows
tdesktop release) and for the producer-tooling installer that targets Windows
operators.

See [`build.md`](build.md) for the platform-agnostic canonical build command.
This document covers Windows-specific prerequisites, the required toolchain,
vcpkg manifest mode, static-linking rationale, and how to verify against the
Linux reference hashes.

---

## Prerequisites

### Visual Studio 2022 Build Tools

Install the **Visual Studio 2022 Build Tools** (or the full Visual Studio 2022
IDE) with the **Desktop development with C++** workload, which includes:

- `cl.exe` 19.3+ (MSVC C/C++ compiler)
- `link.exe`, `dumpbin.exe` (linker + symbol inspector)
- `cmake.exe` ≥ 3.20 (bundled with VS 2022)
- The x64 Native Tools Command Prompt

Download: <https://visualstudio.microsoft.com/visual-cpp-build-tools/>

### vcpkg

vcpkg is the package manager used to provision OpenSSL 3.x.  Bootstrap it once:

```powershell
git clone https://github.com/microsoft/vcpkg.git C:\vcpkg
C:\vcpkg\bootstrap-vcpkg.bat -disableMetrics
```

Or use the pre-installed copy on GitHub Actions runners, available at
`$env:VCPKG_INSTALLATION_ROOT` (typically `C:\vcpkg`).

---

## MSVC vs MinGW Policy

**MSVC is mandatory.  MinGW/MSYS2 is out of scope.**

Rationale: the downstream consumers (tdesktop Windows build, producer-tooling
Windows installer) are all built with MSVC.  vcpkg + MSVC is the
stock-toolchain combination documented by Microsoft and understood by every
Windows developer reading these docs.  Adding MinGW support would double the
CI matrix without benefit for any downstream consumer.

---

## vcpkg Manifest Mode

libteleproto3 ships a [`vcpkg.json`](../vcpkg.json) manifest that declares
`openssl` as a dependency:

```json
{
  "name": "teleproto3",
  "version-semver": "0.1.0",
  "dependencies": ["openssl"]
}
```

[`vcpkg-configuration.json`](../vcpkg-configuration.json) pins the registry
baseline so that OpenSSL 3.x is resolved reproducibly.  To update the baseline
to the latest stable:

```powershell
cd teleproto3/lib
$env:VCPKG_ROOT = "C:\vcpkg"
& "$env:VCPKG_ROOT\vcpkg" x-update-baseline
```

Commit the resulting change to `vcpkg-configuration.json`.

---

## Build Invocation

Run the following from a **Developer Command Prompt for VS 2022** (x64), or
from any shell where `$env:VCPKG_INSTALLATION_ROOT` is set:

```powershell
cmake -S teleproto3/lib -B build `
  -G "Visual Studio 17 2022" -A x64 `
  -DCMAKE_BUILD_TYPE=Release `
  -DT3_CSPRNG_BACKEND=windows `
  -DCMAKE_TOOLCHAIN_FILE="$env:VCPKG_INSTALLATION_ROOT/scripts/buildsystems/vcpkg.cmake" `
  -DVCPKG_TARGET_TRIPLET=x64-windows-static

cmake --build build --config Release
```

Output: `build\Release\teleproto3.lib`

Compiler flags applied by CMake: `/W4 /WX /std:c11` (zero warnings = error).

**Note:** The first configure run triggers vcpkg to install OpenSSL and its
transitive dependencies into `teleproto3/lib/vcpkg_installed/`.  Subsequent
runs use the cached installation.

---

## Static Linking Policy

The `x64-windows-static` triplet links OpenSSL statically into
`teleproto3.lib`.  There is no runtime dependency on `libcrypto-3-x64.dll`.

Rationale: the producer-tooling installer ships as a single `.exe` to
operators who may not have OpenSSL installed system-wide.  Static linking
eliminates "missing DLL" failures at deployment time.  The cost is binary size
(~3-5 MB larger); acceptable for the target audience.

---

## Running Tests

```powershell
ctest --test-dir build --build-config Release --output-on-failure
```

Tests registered:

| Name                  | What it checks                                   |
|-----------------------|--------------------------------------------------|
| `csprng_windows`      | BCryptGenRandom chi-square ±5σ on 1 MiB sample   |
| `kdf_vectors_windows` | KDF output byte-matches `linux-reference.sha256` |

---

## Verification Against Linux Reference

After a successful build, verify that the Windows x64 KDF output is
byte-equivalent to the Linux reference hashes:

```powershell
.\build\Release\run_kdf_vectors.exe `
    teleproto3\conformance\vectors\kdf-kat.txt
```

Compare the output against `teleproto3/lib/tests/vectors/linux-reference.sha256`
(ignoring `#` comment lines).  Any difference indicates a wire-format bug —
the story does not close until the diff is clean.

The CI job (`lib-portability.yml`, `windows` cell) performs this check
automatically on every push.

---

## ABI Symbol Check

After building, verify that the exported symbol set matches the committed
baseline:

```bash
# From a Developer Command Prompt (bash-via-Git-Bash or similar):
bash teleproto3/lib/tests/abi/symbol_list_check.sh \
  build/Release/teleproto3.lib \
  teleproto3/lib/tests/abi/symbol_list.txt
```

To regenerate the baseline after an intentional ABI change, run the same
filter pipeline the script uses (so the baseline format matches what the
guard later compares against). From a Git Bash shell with MSVC in PATH:

```bash
dumpbin //SYMBOLS build/Release/teleproto3.lib \
  | awk '/[[:space:]]External[[:space:]]/ && /\|/ && $0 !~ /[[:space:]]UNDEF[[:space:]]/ {
        idx = index($0, "|")
        rest = substr($0, idx + 1)
        sub(/^[[:space:]]+/, "", rest)
        sym = rest
        sub(/[[:space:]].*/, "", sym)
        if (sym ~ /^t3_/) print sym
      }' \
  | LC_ALL=C sort -u > teleproto3/lib/tests/abi/symbol_list.txt
```

The `UNDEF` rejection is critical — it filters out external *references*
(symbols this archive consumes from elsewhere) and keeps only external
*definitions* (symbols this archive exports).  Mirrors `nm --defined-only`
on Linux/macOS.

Review and commit `tests/abi/symbol_list.txt`.  Re-run `symbol_list_check.sh`
to confirm a clean diff before pushing.

---

## Downstream Consumers

- **Story 2.8** (Windows tdesktop release) — the first downstream consumer of
  `teleproto3.lib` on Windows.  Consumes the static archive directly from a
  CMake `add_subdirectory` or `find_package` invocation.  CMake propagates
  `T3_STATIC_LIB` as a PUBLIC compile definition (see `lib/CMakeLists.txt`),
  which expands `T3_API` to an empty token in consumer translation units —
  the correct linkage for static-archive consumption.  `T3_CONSUMER` is the
  *separate* knob for dynamic-library (DLL) consumption (expands `T3_API`
  to `__declspec(dllimport)`); 1-14 ships only the static path, so DLL-mode
  consumers are out of scope here.
- **Producer-tooling installer** (Epic 3 / Epic 8 scope) — ships a
  Windows-native `.exe` that links `teleproto3.lib` statically.  The
  installer scope is deferred until first non-stakeholder operator demand.

Cross-references: [`build.md`](build.md) (canonical),
[`build-linux.md`](build-linux.md) (Linux reference platform, story 1-12),
[`build-macos.md`](build-macos.md) (macOS, story 1-13).
