#!/usr/bin/env bash
# build-ios.sh — cross-compile libteleproto3.a for iOS device and simulator.
#
# Usage:
#   ci/build-ios.sh --out DIR [--openssl-root DIR]
#
#   --out DIR           Required. Output root directory.
#                       Artifacts:
#                         OUT/device-arm64/libteleproto3.a
#                         OUT/device-arm64/include/
#                         OUT/simulator/libteleproto3.a
#                         OUT/simulator/include/
#
#   --openssl-root DIR  Optional. Pre-built OpenSSL for iOS (device + simulator
#                       as subdirs or a single include/).  When absent, this script
#                       downloads OpenSSL 3.3.2 source and cross-compiles it using
#                       xcrun toolchains.
#
# Requirements:
#   - macOS host with Xcode installed (xcrun must be available)
#   - cmake >= 3.20
#
# Design notes:
#   - iOS CSPRNG uses t3_csprng_macos.c (SecRandomCopyBytes from Security.framework)
#   - Device slice: iphoneos SDK, arm64 only
#   - Simulator slice: iphonesimulator SDK, arm64 + x86_64 (fat via lipo)

set -euo pipefail

# ── Constants ──────────────────────────────────────────────────────────────────
OPENSSL_VERSION="3.3.2"
OPENSSL_URL="https://www.openssl.org/source/openssl-${OPENSSL_VERSION}.tar.gz"
OPENSSL_SHA256="2e8a40b01979afe8be0bbfb3de5dc1c6709fedb46d6c89c10da114ab5fc3d281"

# ── Argument parsing ───────────────────────────────────────────────────────────
OUT=""
OPENSSL_ROOT=""

usage() {
    printf 'Usage: %s --out DIR [--openssl-root DIR]\n' "$(basename "$0")" >&2
    printf '  --out DIR           Output directory\n' >&2
    printf '  --openssl-root DIR  Pre-built OpenSSL (optional; downloads + builds if absent)\n' >&2
    exit 2
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --out)          OUT="${2:?'--out requires a path'}";           shift 2 ;;
        --openssl-root) OPENSSL_ROOT="${2:?'--openssl-root requires a path'}"; shift 2 ;;
        -h|--help)      usage ;;
        *)
            printf 'error: unknown argument: %s\n' "$1" >&2
            usage
            ;;
    esac
done

[[ -n "$OUT" ]] || { printf 'error: --out is required\n' >&2; usage; }

# ── Pre-flight checks ──────────────────────────────────────────────────────────
for cmd in cmake xcrun lipo; do
    command -v "$cmd" >/dev/null 2>&1 || {
        printf 'error: required tool not found: %s\n' "$cmd" >&2; exit 1
    }
done

# Verify iOS SDK is available
if ! xcrun --sdk iphoneos --show-sdk-path >/dev/null 2>&1; then
    printf 'error: Xcode iOS SDK (iphoneos) not found.\n' >&2
    printf '       Install Xcode with iOS platform support.\n' >&2
    exit 1
fi
if ! xcrun --sdk iphonesimulator --show-sdk-path >/dev/null 2>&1; then
    printf 'error: Xcode iOS Simulator SDK (iphonesimulator) not found.\n' >&2
    printf '       Install Xcode with iOS platform support.\n' >&2
    exit 1
fi

# ── Locate lib/CMakeLists.txt relative to this script ─────────────────────────
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
LIB_DIR="$(cd "$SCRIPT_DIR/../lib" && pwd)"

[[ -f "$LIB_DIR/CMakeLists.txt" ]] || {
    printf 'error: lib/CMakeLists.txt not found at: %s\n' "$LIB_DIR" >&2; exit 1
}

INCLUDE_SRC="$LIB_DIR/include"

mkdir -p "$OUT"
OUT="$(cd "$OUT" && pwd)"

# ── OpenSSL cross-compile for iOS ──────────────────────────────────────────────
build_openssl_ios() {
    local SLICE="$1"    # "device" or "simulator"
    local SSL_OUT="$2"  # output dir for this slice

    if [[ -d "$SSL_OUT" && -f "$SSL_OUT/lib/libssl.a" && -f "$SSL_OUT/lib/libcrypto.a" ]]; then
        printf 'OpenSSL %s: already built, skipping\n' "$SLICE"
        return
    fi

    local TMPDIR_SSL
    TMPDIR_SSL="$(mktemp -d "/tmp/openssl-ios-src-XXXXXX")"
    trap 'rm -rf "$TMPDIR_SSL"' RETURN

    printf '\n=== Downloading OpenSSL %s ===\n' "$OPENSSL_VERSION"
    local TARBALL="$TMPDIR_SSL/openssl.tar.gz"
    curl -fSL "$OPENSSL_URL" -o "$TARBALL"

    # Verify checksum
    local ACTUAL_SHA256
    ACTUAL_SHA256="$(shasum -a 256 "$TARBALL" | awk '{print $1}')"
    if [[ "$ACTUAL_SHA256" != "$OPENSSL_SHA256" ]]; then
        printf 'error: OpenSSL tarball checksum mismatch\n' >&2
        printf '       expected: %s\n' "$OPENSSL_SHA256" >&2
        printf '       actual:   %s\n' "$ACTUAL_SHA256" >&2
        exit 1
    fi

    tar -xzf "$TARBALL" -C "$TMPDIR_SSL"
    local SRC_DIR="$TMPDIR_SSL/openssl-${OPENSSL_VERSION}"

    mkdir -p "$SSL_OUT"

    local IPHONEOS_SDK
    IPHONEOS_SDK="$(xcrun --sdk iphoneos --show-sdk-path)"
    local IOSSIM_SDK
    IOSSIM_SDK="$(xcrun --sdk iphonesimulator --show-sdk-path)"

    printf '\n=== Building OpenSSL %s for iOS %s ===\n' "$OPENSSL_VERSION" "$SLICE"

    if [[ "$SLICE" == "device" ]]; then
        # arm64 device (ios64-cross target)
        export CROSS_TOP
        CROSS_TOP="$(xcrun --sdk iphoneos --show-sdk-platform-path)/Developer"
        export CROSS_SDK
        CROSS_SDK="$(basename "$IPHONEOS_SDK")"

        (
            cd "$SRC_DIR"
            ./Configure ios64-cross no-shared no-tests \
                --prefix="$SSL_OUT" \
                --openssldir="$SSL_OUT/ssl"
            make -j"$(sysctl -n hw.logicalcpu)" build_libs
            make install_dev
        )
        unset CROSS_TOP CROSS_SDK

    elif [[ "$SLICE" == "simulator" ]]; then
        # Simulator: build arm64 and x86_64 separately then combine with lipo.
        local ARM64_OUT="$TMPDIR_SSL/ssl-sim-arm64"
        local X86_OUT="$TMPDIR_SSL/ssl-sim-x86_64"

        # arm64 simulator
        (
            export CROSS_TOP
            CROSS_TOP="$(xcrun --sdk iphonesimulator --show-sdk-platform-path)/Developer"
            export CROSS_SDK
            CROSS_SDK="$(basename "$IOSSIM_SDK")"
            cd "$SRC_DIR"
            make distclean 2>/dev/null || true
            ./Configure iossimulator-xcrun no-shared no-tests \
                --prefix="$ARM64_OUT" \
                --openssldir="$ARM64_OUT/ssl"
            # Force arm64 arch
            perl -i -pe 's/^(CFLAGS\s*=)/$1 -arch arm64 /' Makefile
            make -j"$(sysctl -n hw.logicalcpu)" build_libs
            make install_dev
        )

        # x86_64 simulator
        (
            export CROSS_TOP
            CROSS_TOP="$(xcrun --sdk iphonesimulator --show-sdk-platform-path)/Developer"
            export CROSS_SDK
            CROSS_SDK="$(basename "$IOSSIM_SDK")"
            cd "$SRC_DIR"
            make distclean 2>/dev/null || true
            ./Configure iossimulator-xcrun no-shared no-tests \
                --prefix="$X86_OUT" \
                --openssldir="$X86_OUT/ssl"
            # Force x86_64 arch
            perl -i -pe 's/^(CFLAGS\s*=)/$1 -arch x86_64 /' Makefile
            make -j"$(sysctl -n hw.logicalcpu)" build_libs
            make install_dev
        )

        # Combine into fat libraries
        mkdir -p "$SSL_OUT/lib" "$SSL_OUT/include"
        lipo -create \
            "$ARM64_OUT/lib/libssl.a" \
            "$X86_OUT/lib/libssl.a" \
            -output "$SSL_OUT/lib/libssl.a"
        lipo -create \
            "$ARM64_OUT/lib/libcrypto.a" \
            "$X86_OUT/lib/libcrypto.a" \
            -output "$SSL_OUT/lib/libcrypto.a"
        cp -r "$ARM64_OUT/include" "$SSL_OUT/"
    fi

    printf 'OpenSSL %s built: %s\n' "$SLICE" "$SSL_OUT"
}

# ── Determine OpenSSL paths ────────────────────────────────────────────────────
OPENSSL_DEVICE_ROOT=""
OPENSSL_SIM_ROOT=""

if [[ -n "$OPENSSL_ROOT" ]]; then
    # Caller supplied a pre-built root.  Expect either:
    #   OPENSSL_ROOT/{device,simulator}/  — split by slice
    #   OPENSSL_ROOT/                     — single (reused for both slices)
    if [[ -d "$OPENSSL_ROOT/device" ]]; then
        OPENSSL_DEVICE_ROOT="$OPENSSL_ROOT/device"
        OPENSSL_SIM_ROOT="$OPENSSL_ROOT/simulator"
    else
        OPENSSL_DEVICE_ROOT="$OPENSSL_ROOT"
        OPENSSL_SIM_ROOT="$OPENSSL_ROOT"
    fi
else
    # Build OpenSSL from source
    OPENSSL_CACHE_DIR="$OUT/.openssl-cache"
    OPENSSL_DEVICE_ROOT="$OPENSSL_CACHE_DIR/device"
    OPENSSL_SIM_ROOT="$OPENSSL_CACHE_DIR/simulator"
    build_openssl_ios "device"    "$OPENSSL_DEVICE_ROOT"
    build_openssl_ios "simulator" "$OPENSSL_SIM_ROOT"
fi

# ── Helper: cmake build for one slice ─────────────────────────────────────────
build_slice() {
    local SLICE_NAME="$1"        # "device-arm64" or "simulator"
    local SDK="$2"               # iphoneos or iphonesimulator
    local ARCHS="$3"             # "arm64" or "arm64;x86_64"
    local SSL_ROOT="$4"          # OpenSSL root for this slice

    printf '\n=== Building libteleproto3 for iOS %s ===\n' "$SLICE_NAME"

    local BUILD_DIR
    BUILD_DIR="$(mktemp -d "/tmp/t3-ios-${SLICE_NAME}-XXXXXX")"
    trap 'rm -rf "$BUILD_DIR"' RETURN

    local SLICE_OUT="$OUT/$SLICE_NAME"
    mkdir -p "$SLICE_OUT"

    # Locate OpenSSL libraries (support lib/ or lib64/)
    local CRYPTO_LIB SSL_LIB INC_DIR
    if [[ -f "$SSL_ROOT/lib/libcrypto.a" ]]; then
        CRYPTO_LIB="$SSL_ROOT/lib/libcrypto.a"
        SSL_LIB="$SSL_ROOT/lib/libssl.a"
    elif [[ -f "$SSL_ROOT/lib64/libcrypto.a" ]]; then
        CRYPTO_LIB="$SSL_ROOT/lib64/libcrypto.a"
        SSL_LIB="$SSL_ROOT/lib64/libssl.a"
    else
        printf 'error: libcrypto.a not found under %s/lib[64]/\n' "$SSL_ROOT" >&2
        exit 1
    fi
    INC_DIR="$SSL_ROOT/include"

    cmake \
        -S "$LIB_DIR" \
        -B "$BUILD_DIR" \
        -DCMAKE_SYSTEM_NAME=iOS \
        -DCMAKE_OSX_DEPLOYMENT_TARGET=13.0 \
        -DCMAKE_OSX_SYSROOT="$SDK" \
        -DCMAKE_OSX_ARCHITECTURES="$ARCHS" \
        -DCMAKE_BUILD_TYPE=Release \
        -DT3_BUILD_CLIENT=ON \
        -DT3_BUILD_TESTS=OFF \
        -DT3_SHIM_SOCKS5=ON \
        -DT3_CSPRNG_BACKEND=macos \
        -DT3_HARDENING=OFF \
        -DOPENSSL_INCLUDE_DIR="$INC_DIR" \
        -DOPENSSL_SSL_LIBRARY="$SSL_LIB" \
        -DOPENSSL_CRYPTO_LIBRARY="$CRYPTO_LIB"

    cmake --build "$BUILD_DIR" --target teleproto3 \
        --parallel "$(sysctl -n hw.logicalcpu 2>/dev/null || echo 2)"

    # Locate artifact
    local ARTIFACT=""
    for candidate in \
        "$BUILD_DIR/libteleproto3.a" \
        "$BUILD_DIR/lib/libteleproto3.a" \
        "$BUILD_DIR/teleproto3/libteleproto3.a"
    do
        [[ -f "$candidate" ]] && { ARTIFACT="$candidate"; break; }
    done

    [[ -n "$ARTIFACT" ]] || {
        printf 'error: libteleproto3.a not found after build (slice=%s)\n' "$SLICE_NAME" >&2
        exit 1
    }

    cp "$ARTIFACT" "$SLICE_OUT/libteleproto3.a"
    cp -r "$INCLUDE_SRC" "$SLICE_OUT/include"
    rm -rf "$SLICE_OUT/include/internal"

    printf 'Slice %s: %s\n' "$SLICE_NAME" "$SLICE_OUT/libteleproto3.a"
}

# ── Build device slice ─────────────────────────────────────────────────────────
build_slice "device-arm64" "iphoneos" "arm64" "$OPENSSL_DEVICE_ROOT"

# ── Build simulator fat slice ──────────────────────────────────────────────────
# Build arm64 and x86_64 simulator slices individually then lipo them together.
# CMake/Xcode may refuse to build a fat static lib for the simulator in one
# invocation on older toolchains, so we use the reliable split+lipo approach.

SIM_ARM64_BUILD="$(mktemp -d /tmp/t3-sim-arm64-XXXXXX)"
SIM_X86_BUILD="$(mktemp -d /tmp/t3-sim-x86-XXXXXX)"
trap 'rm -rf "$SIM_ARM64_BUILD" "$SIM_X86_BUILD"' EXIT

printf '\n=== Building libteleproto3 simulator arm64 ===\n'

CRYPTO_LIB_SIM=""
SSL_LIB_SIM=""
INC_SIM=""
if [[ -f "$OPENSSL_SIM_ROOT/lib/libcrypto.a" ]]; then
    CRYPTO_LIB_SIM="$OPENSSL_SIM_ROOT/lib/libcrypto.a"
    SSL_LIB_SIM="$OPENSSL_SIM_ROOT/lib/libssl.a"
elif [[ -f "$OPENSSL_SIM_ROOT/lib64/libcrypto.a" ]]; then
    CRYPTO_LIB_SIM="$OPENSSL_SIM_ROOT/lib64/libcrypto.a"
    SSL_LIB_SIM="$OPENSSL_SIM_ROOT/lib64/libssl.a"
else
    printf 'error: simulator OpenSSL libcrypto.a not found under %s\n' "$OPENSSL_SIM_ROOT" >&2
    exit 1
fi
INC_SIM="$OPENSSL_SIM_ROOT/include"

cmake \
    -S "$LIB_DIR" \
    -B "$SIM_ARM64_BUILD" \
    -DCMAKE_SYSTEM_NAME=iOS \
    -DCMAKE_OSX_DEPLOYMENT_TARGET=13.0 \
    -DCMAKE_OSX_SYSROOT=iphonesimulator \
    -DCMAKE_OSX_ARCHITECTURES=arm64 \
    -DCMAKE_BUILD_TYPE=Release \
    -DT3_BUILD_CLIENT=ON \
    -DT3_BUILD_TESTS=OFF \
    -DT3_CSPRNG_BACKEND=macos \
    -DT3_HARDENING=OFF \
    -DOPENSSL_INCLUDE_DIR="$INC_SIM" \
    -DOPENSSL_SSL_LIBRARY="$SSL_LIB_SIM" \
    -DOPENSSL_CRYPTO_LIBRARY="$CRYPTO_LIB_SIM"

cmake --build "$SIM_ARM64_BUILD" --target teleproto3 \
    --parallel "$(sysctl -n hw.logicalcpu 2>/dev/null || echo 2)"

printf '\n=== Building libteleproto3 simulator x86_64 ===\n'

cmake \
    -S "$LIB_DIR" \
    -B "$SIM_X86_BUILD" \
    -DCMAKE_SYSTEM_NAME=iOS \
    -DCMAKE_OSX_DEPLOYMENT_TARGET=13.0 \
    -DCMAKE_OSX_SYSROOT=iphonesimulator \
    -DCMAKE_OSX_ARCHITECTURES=x86_64 \
    -DCMAKE_BUILD_TYPE=Release \
    -DT3_BUILD_CLIENT=ON \
    -DT3_BUILD_TESTS=OFF \
    -DT3_CSPRNG_BACKEND=macos \
    -DT3_HARDENING=OFF \
    -DOPENSSL_INCLUDE_DIR="$INC_SIM" \
    -DOPENSSL_SSL_LIBRARY="$SSL_LIB_SIM" \
    -DOPENSSL_CRYPTO_LIBRARY="$CRYPTO_LIB_SIM"

cmake --build "$SIM_X86_BUILD" --target teleproto3 \
    --parallel "$(sysctl -n hw.logicalcpu 2>/dev/null || echo 2)"

# Locate each slice artifact
find_artifact() {
    local BUILD="$1"
    for candidate in \
        "$BUILD/libteleproto3.a" \
        "$BUILD/lib/libteleproto3.a" \
        "$BUILD/teleproto3/libteleproto3.a"
    do
        [[ -f "$candidate" ]] && { printf '%s' "$candidate"; return; }
    done
    printf 'error: libteleproto3.a not found in %s\n' "$BUILD" >&2
    exit 1
}

SIM_ARM64_A="$(find_artifact "$SIM_ARM64_BUILD")"
SIM_X86_A="$(find_artifact "$SIM_X86_BUILD")"

printf '\n=== Combining simulator slices with lipo ===\n'
SIM_OUT="$OUT/simulator"
mkdir -p "$SIM_OUT"

lipo -create "$SIM_ARM64_A" "$SIM_X86_A" -output "$SIM_OUT/libteleproto3.a"
cp -r "$INCLUDE_SRC" "$SIM_OUT/include"
rm -rf "$SIM_OUT/include/internal"

# ── Summary ────────────────────────────────────────────────────────────────────
printf '\n=== build-ios.sh complete ===\n'
printf 'Artifacts:\n'
for f in \
    "$OUT/device-arm64/libteleproto3.a" \
    "$OUT/simulator/libteleproto3.a"
do
    if [[ -f "$f" ]]; then
        SIZE="$(wc -c < "$f" | tr -d ' ')"
        printf '  %s  (%s bytes)\n' "$f" "$SIZE"
        file "$f" || true
    else
        printf '  MISSING: %s\n' "$f"
    fi
done
