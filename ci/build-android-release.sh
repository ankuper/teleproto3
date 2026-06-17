#!/usr/bin/env bash
# build-android-release.sh — CI variant: cross-compile libteleproto3.a for
# Android ABIs using NDK + OpenSSL built from source (no Telegram-Android dep).
#
# Usage:
#   ci/build-android-release.sh \
#     [--ndk  DIR]           # NDK root; defaults to $ANDROID_NDK_ROOT
#     --out   DIR            # required: output root
#     [--abis "LIST"]        # space-separated ABI names (default: all 4)
#
# Output layout:
#   OUT/arm64-v8a/libteleproto3.a
#   OUT/arm64-v8a/include/
#   OUT/armeabi-v7a/libteleproto3.a
#   ...
#
# Each ABI's OpenSSL is cross-compiled from source using the NDK Clang toolchain.
# Compiled OpenSSL binaries are cached under OUT/.openssl-cache/{ABI}/ so
# repeated runs (e.g. local re-runs) don't rebuild from scratch.
#
# Notes:
#   - CMAKE_SYSTEM_NAME=Android (set by NDK toolchain) would defeat AUTO CSPRNG;
#     we pass -DT3_CSPRNG_BACKEND=linux explicitly (same as build-android.sh).
#   - T3_HARDENING=OFF avoids -D_FORTIFY_SOURCE=2 friction with NDK sysroot.
#   - Tested against NDK r26d; minimum NDK version: r21.

set -euo pipefail

# ── Constants ──────────────────────────────────────────────────────────────────
OPENSSL_VERSION="3.3.2"
OPENSSL_URL="https://www.openssl.org/source/openssl-${OPENSSL_VERSION}.tar.gz"
OPENSSL_SHA256="2e8a40b01979afe8be0bbfb3de5dc1c6709fedb46d6c89f9e3583a19c9e1c721"
ANDROID_PLATFORM="android-21"
DEFAULT_ABIS="arm64-v8a armeabi-v7a x86 x86_64"

# ── Argument parsing ───────────────────────────────────────────────────────────
NDK="${ANDROID_NDK_ROOT:-}"
OUT=""
ABIS="$DEFAULT_ABIS"

usage() {
    printf 'Usage: %s [--ndk DIR] --out DIR [--abis "LIST"]\n' "$(basename "$0")" >&2
    printf '  --ndk  DIR   NDK root (defaults to $ANDROID_NDK_ROOT)\n' >&2
    printf '  --out  DIR   Output directory\n' >&2
    printf '  --abis LIST  Space-separated ABI list (default: %s)\n' "$DEFAULT_ABIS" >&2
    exit 2
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --ndk)  NDK="${2:?'--ndk requires a path'}";  shift 2 ;;
        --out)  OUT="${2:?'--out requires a path'}";  shift 2 ;;
        --abis) ABIS="${2:?'--abis requires a value'}"; shift 2 ;;
        -h|--help) usage ;;
        *)
            printf 'error: unknown argument: %s\n' "$1" >&2
            usage
            ;;
    esac
done

[[ -n "$NDK" ]] || { printf 'error: --ndk is required (or set $ANDROID_NDK_ROOT)\n' >&2; usage; }
[[ -n "$OUT" ]] || { printf 'error: --out is required\n' >&2; usage; }

# ── Validate NDK ───────────────────────────────────────────────────────────────
TOOLCHAIN_FILE="$NDK/build/cmake/android.toolchain.cmake"

[[ -d "$NDK" ]] || { printf 'error: NDK directory not found: %s\n' "$NDK" >&2; exit 1; }
[[ -f "$TOOLCHAIN_FILE" ]] || {
    printf 'error: NDK toolchain not found: %s\n' "$TOOLCHAIN_FILE" >&2; exit 1
}

command -v cmake >/dev/null 2>&1 || { printf 'error: cmake not found in PATH\n' >&2; exit 1; }

# ── Locate lib/ relative to this script ───────────────────────────────────────
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
LIB_DIR="$(cd "$SCRIPT_DIR/../lib" && pwd)"

[[ -f "$LIB_DIR/CMakeLists.txt" ]] || {
    printf 'error: lib/CMakeLists.txt not found at: %s\n' "$LIB_DIR" >&2; exit 1
}

INCLUDE_SRC="$LIB_DIR/include"

mkdir -p "$OUT"
OUT="$(cd "$OUT" && pwd)"

# ── OpenSSL ABI → NDK target mapping ──────────────────────────────────────────
# Maps Android ABI name to OpenSSL Configure target and NDK API-level triplet.
openssl_target_for_abi() {
    case "$1" in
        arm64-v8a)   printf 'android-arm64' ;;
        armeabi-v7a) printf 'android-arm'   ;;
        x86)         printf 'android-x86'   ;;
        x86_64)      printf 'android-x86_64';;
        *) printf 'error: unknown ABI %s\n' "$1" >&2; exit 1 ;;
    esac
}

# ── Build OpenSSL for one ABI ──────────────────────────────────────────────────
build_openssl_android() {
    local ABI="$1"
    local SSL_OUT="$2"

    if [[ -d "$SSL_OUT" && -f "$SSL_OUT/lib/libssl.a" && -f "$SSL_OUT/lib/libcrypto.a" ]]; then
        printf 'OpenSSL %s: cached, skipping\n' "$ABI"
        return
    fi

    local OPENSSL_TARGET
    OPENSSL_TARGET="$(openssl_target_for_abi "$ABI")"

    # NDK Clang toolchain path
    local NDK_LLVM="$NDK/toolchains/llvm/prebuilt/linux-x86_64"
    if [[ ! -d "$NDK_LLVM" ]]; then
        # macOS NDK layout
        NDK_LLVM="$NDK/toolchains/llvm/prebuilt/darwin-x86_64"
    fi
    [[ -d "$NDK_LLVM" ]] || {
        printf 'error: NDK LLVM toolchain not found under %s\n' "$NDK" >&2; exit 1
    }

    printf '\n=== Building OpenSSL %s for Android ABI=%s ===\n' "$OPENSSL_VERSION" "$ABI"

    local TMPDIR_SSL
    TMPDIR_SSL="$(mktemp -d "/tmp/openssl-android-${ABI}-XXXXXX")"
    trap 'rm -rf "$TMPDIR_SSL"' RETURN

    local TARBALL="$TMPDIR_SSL/openssl.tar.gz"

    # Download (only once — reuse across ABI iterations if tarball is shared)
    local TARBALL_CACHE="$OUT/.openssl-cache/openssl-${OPENSSL_VERSION}.tar.gz"
    mkdir -p "$(dirname "$TARBALL_CACHE")"
    if [[ ! -f "$TARBALL_CACHE" ]]; then
        printf 'Downloading OpenSSL %s...\n' "$OPENSSL_VERSION"
        curl -fSL "$OPENSSL_URL" -o "$TARBALL_CACHE"
        local ACTUAL_SHA256
        ACTUAL_SHA256="$(sha256sum "$TARBALL_CACHE" 2>/dev/null | awk '{print $1}' \
                        || shasum -a 256 "$TARBALL_CACHE" | awk '{print $1}')"
        if [[ "$ACTUAL_SHA256" != "$OPENSSL_SHA256" ]]; then
            printf 'error: OpenSSL tarball checksum mismatch\n' >&2
            printf '       expected: %s\n' "$OPENSSL_SHA256" >&2
            printf '       actual:   %s\n' "$ACTUAL_SHA256" >&2
            rm -f "$TARBALL_CACHE"
            exit 1
        fi
    fi
    cp "$TARBALL_CACHE" "$TARBALL"

    tar -xzf "$TARBALL" -C "$TMPDIR_SSL"
    local SRC_DIR="$TMPDIR_SSL/openssl-${OPENSSL_VERSION}"

    mkdir -p "$SSL_OUT"

    (
        cd "$SRC_DIR"

        # Export ANDROID_NDK_ROOT so OpenSSL's Configurations/10-main.conf finds it
        export ANDROID_NDK_ROOT="$NDK"
        export PATH="$NDK_LLVM/bin:$PATH"

        ./Configure "$OPENSSL_TARGET" no-shared no-tests \
            -D__ANDROID_API__=21 \
            --prefix="$SSL_OUT" \
            --openssldir="$SSL_OUT/ssl"

        make -j"$(nproc 2>/dev/null || sysctl -n hw.logicalcpu 2>/dev/null || echo 2)" build_libs
        make install_dev
    )

    printf 'OpenSSL for %s: done → %s\n' "$ABI" "$SSL_OUT"
}

# ── Per-ABI build ──────────────────────────────────────────────────────────────
printf '\n=== build-android-release.sh ===\n'
printf 'NDK:    %s\n' "$NDK"
printf 'Output: %s\n' "$OUT"
printf 'ABIs:   %s\n\n' "$ABIS"

read -ra ABI_LIST <<< "$ABIS"
BUILT=()

for ABI in "${ABI_LIST[@]}"; do
    ABI="$(printf '%s' "$ABI" | tr -d '[:space:]')"
    [[ -n "$ABI" ]] || continue

    printf '\n--- ABI: %s ---\n' "$ABI"

    # Build (or use cached) OpenSSL for this ABI
    SSL_OUT="$OUT/.openssl-cache/$ABI"
    build_openssl_android "$ABI" "$SSL_OUT"

    CRYPTO_LIB="$SSL_OUT/lib/libcrypto.a"
    SSL_LIB="$SSL_OUT/lib/libssl.a"
    INC_DIR="$SSL_OUT/include"

    [[ -f "$CRYPTO_LIB" ]] || {
        printf 'error: libcrypto.a not found: %s\n' "$CRYPTO_LIB" >&2; exit 1
    }
    [[ -f "$SSL_LIB" ]] || {
        printf 'error: libssl.a not found: %s\n' "$SSL_LIB" >&2; exit 1
    }

    BUILD_DIR="$(mktemp -d "/tmp/teleproto3-android-${ABI}-XXXXXX")"

    printf 'Configure teleproto3 for %s...\n' "$ABI"
    cmake \
        -S "$LIB_DIR" \
        -B "$BUILD_DIR" \
        -DCMAKE_TOOLCHAIN_FILE="$TOOLCHAIN_FILE" \
        -DANDROID_ABI="$ABI" \
        -DANDROID_PLATFORM="$ANDROID_PLATFORM" \
        -DT3_BUILD_CLIENT=ON \
        -DT3_BUILD_TESTS=OFF \
        -DT3_CSPRNG_BACKEND=linux \
        -DT3_HARDENING=OFF \
        -DOPENSSL_INCLUDE_DIR="$INC_DIR" \
        -DOPENSSL_SSL_LIBRARY="$SSL_LIB" \
        -DOPENSSL_CRYPTO_LIBRARY="$CRYPTO_LIB" \
        -DCMAKE_BUILD_TYPE=Release

    printf 'Build teleproto3 for %s...\n' "$ABI"
    cmake --build "$BUILD_DIR" --target teleproto3 \
        --parallel "$(nproc 2>/dev/null || sysctl -n hw.logicalcpu 2>/dev/null || echo 2)"

    # Locate artifact
    ARTIFACT=""
    for candidate in \
        "$BUILD_DIR/libteleproto3.a" \
        "$BUILD_DIR/lib/libteleproto3.a" \
        "$BUILD_DIR/teleproto3/libteleproto3.a"
    do
        [[ -f "$candidate" ]] && { ARTIFACT="$candidate"; break; }
    done

    [[ -n "$ARTIFACT" ]] || {
        printf 'error: libteleproto3.a not found after build (ABI=%s)\n' "$ABI" >&2; exit 1
    }

    ABI_OUT="$OUT/$ABI"
    mkdir -p "$ABI_OUT"
    cp "$ARTIFACT" "$ABI_OUT/libteleproto3.a"
    cp -r "$INCLUDE_SRC" "$ABI_OUT/include"
    rm -rf "$ABI_OUT/include/internal"
    rm -rf "$BUILD_DIR"

    BUILT+=("$ABI_OUT/libteleproto3.a")
    printf 'ABI %s: OK → %s\n' "$ABI" "$ABI_OUT/libteleproto3.a"
done

# ── Summary ────────────────────────────────────────────────────────────────────
printf '\n=== build-android-release.sh complete ===\n'
printf 'Output files:\n'
for f in "${BUILT[@]}"; do
    SIZE="$(wc -c < "$f" | tr -d ' ')"
    printf '  %s  (%s bytes)\n' "$f" "$SIZE"
done
