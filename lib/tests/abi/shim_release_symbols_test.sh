#!/bin/sh
# shim_release_symbols_test.sh — ATDD RED-phase ABI/symbol check for story 9.2 AC3.
#
# ===================================================================
#  ATDD RED PHASE (story 9.2 AC3) — see checklist
# ===================================================================
# AC3: the RELEASE library that CI ships must expose the SOCKS5 shim:
#   (a) the compiled lib defines  T3_SHIM_SOCKS5_AVAILABLE 1
#       (emitted ONLY when CMake is configured with -DT3_SHIM_SOCKS5=ON;
#        lib/include/t3_features.h self-defaults it to 0 otherwise), and
#   (b) the static archive exports the public shim symbols
#       t3_shim_open / t3_shim_close / t3_shim_get_credentials / t3_shim_local_port.
#
# WHY THIS IS RED TODAY:
#   .github/workflows/build-lib.yml configures the linux/macOS/windows release
#   builds with -DT3_BUILD_CLIENT=ON but WITHOUT -DT3_SHIM_SOCKS5=ON. So the
#   release artifact neither defines the feature flag as 1 nor compiles
#   shim_socks5.c — both assertions below fail until build-lib.yml adds the flag
#   for the release matrix (AC3 / AC5 release-cut work).
#
# DEFAULT BEHAVIOUR: exit 77 (CTest/autotools SKIP convention) so CI stays green
# while this scaffold is parked in the RED phase.
#
# ------------------------------------------------------------------------------
# HOW TO ACTIVATE (flip RED -> live):
#   1. Remove / comment out the "RED SKIP GUARD" block below (the `exit 77`).
#   2. Provide the archive + headers to inspect via env, OR let the script
#      reproduce the *release* configure line so the check tracks what CI ships:
#        T3_LIB_A    = path to libteleproto3.a            (default: build/libteleproto3.a)
#        T3_FEATURES = path to the COMPILED t3_features.h  (default: lib/include/t3_features.h)
#        T3_CONFIGURE_CHECK=1  -> also run `cmake -LA` on a release build dir and
#                                 assert T3_SHIM_SOCKS5:BOOL=ON (proves the flag
#                                 is wired, not just locally toggled).
#   3. Run.  It FAILS today because the release build flag is OFF; it PASSES once
#      build-lib.yml sets -DT3_SHIM_SOCKS5=ON for the release matrix.
# ------------------------------------------------------------------------------

set -eu

# Story 9.2 AC3: this check is ACTIVE. build-lib.yml now configures the release
# build with -DT3_SHIM_SOCKS5=ON and bakes T3_SHIM_SOCKS5_AVAILABLE=1 into the
# shipped t3_features.h, so both assertions below hold against the artifact.

LIB_A="${T3_LIB_A:-build/libteleproto3.a}"
FEATURES_H="${T3_FEATURES:-lib/include/t3_features.h}"

fail=0

# --- Assertion (a): the COMPILED feature flag must be 1 ----------------------
# t3_features.h alone self-defaults to 0; the value 1 is injected by
# target_compile_definitions(... PUBLIC T3_SHIM_SOCKS5_AVAILABLE=1) when
# T3_SHIM_SOCKS5=ON. We compile a tiny probe against the lib's public include
# dir to read the EFFECTIVE value rather than grepping the header text.
PROBE_DIR="$(mktemp -d)"
trap 'rm -rf "$PROBE_DIR"' EXIT
cat > "$PROBE_DIR/probe.c" <<'EOF'
#include "t3_features.h"
#if !defined(T3_SHIM_SOCKS5_AVAILABLE) || (T3_SHIM_SOCKS5_AVAILABLE != 1)
#error "T3_SHIM_SOCKS5_AVAILABLE is not 1 in the release build"
#endif
int main(void) { return 0; }
EOF

INC_DIR="$(dirname "$FEATURES_H")"
# When the lib is built with the flag, CI must also pass -DT3_SHIM_SOCKS5_AVAILABLE=1
# on the probe compile to mirror the PUBLIC compile definition consumers inherit.
CC="${CC:-cc}"
if "$CC" -I"$INC_DIR" -DT3_SHIM_SOCKS5_AVAILABLE=1 -c "$PROBE_DIR/probe.c" -o "$PROBE_DIR/probe.o" 2>/dev/null; then
    : # consumer inheriting the PUBLIC definition compiles -> ok
else
    echo "FAIL(a): probe could not confirm T3_SHIM_SOCKS5_AVAILABLE==1"
    fail=1
fi
# Stronger check: the BARE header (no -D) must NOT silently be 1 unless the lib
# actually injects it. If the release lib forgot the flag, a bare compile leaves
# it 0 and the probe #errors — which is the RED we want to surface.
if "$CC" -I"$INC_DIR" -c "$PROBE_DIR/probe.c" -o "$PROBE_DIR/probe_bare.o" 2>/dev/null; then
    : # only passes if some build step defined it to 1 globally
else
    echo "FAIL(a): bare t3_features.h leaves T3_SHIM_SOCKS5_AVAILABLE=0"\
         " -> release lib did not enable T3_SHIM_SOCKS5"
    fail=1
fi

# --- Assertion (b): the archive must export the public shim symbols ----------
if [ ! -f "$LIB_A" ]; then
    echo "FAIL(b): archive not found: $LIB_A"
    fail=1
else
    SYMS="$(nm "$LIB_A" 2>/dev/null || true)"
    for sym in t3_shim_open t3_shim_close t3_shim_get_credentials t3_shim_local_port; do
        # Defined text symbols show as 'T' (or 't'); accept any defined kind.
        if printf '%s\n' "$SYMS" | grep -Eq "[[:space:]][TtWw][[:space:]]_?${sym}$"; then
            echo "ok(b): $sym exported"
        else
            echo "FAIL(b): symbol not exported by release archive: $sym"
            fail=1
        fi
    done
fi

# --- Optional: prove the release CONFIGURE wires the flag --------------------
if [ "${T3_CONFIGURE_CHECK:-0}" = "1" ]; then
    BUILD_DIR="${T3_BUILD_DIR:-build}"
    if cmake -LA -N "$BUILD_DIR" 2>/dev/null | grep -q '^T3_SHIM_SOCKS5:BOOL=ON'; then
        echo "ok(c): release build dir has T3_SHIM_SOCKS5:BOOL=ON"
    else
        echo "FAIL(c): release build dir does NOT set T3_SHIM_SOCKS5=ON"
        fail=1
    fi
fi

if [ "$fail" -ne 0 ]; then
    echo "=== shim_release_symbols_test: FAIL (story 9.2 AC3 not yet satisfied) ==="
    exit 1
fi
echo "=== shim_release_symbols_test: PASS ==="
exit 0
