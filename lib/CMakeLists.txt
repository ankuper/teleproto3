cmake_minimum_required(VERSION 3.20)
project(teleproto3 LANGUAGES C VERSION 0.1.1)

# -----------------------------------------------------------------------
# Build options (set via -D on the cmake command line)
# -----------------------------------------------------------------------
option(T3_BUILD_TESTS "Build and register test programs via CTest" ON)

# -----------------------------------------------------------------------
# macOS universal binary option (Task 4 — Story 1-13)
# Default OFF: arm64-only is mandatory; x86_64 is best-effort and requires
# a fat OpenSSL bottle. Set ON to build a fat archive.
# -----------------------------------------------------------------------
if(APPLE)
    option(T3_MACOS_X86_64 "Include x86_64 slice in macOS build" OFF)
    if(T3_MACOS_X86_64)
        if(NOT CMAKE_OSX_ARCHITECTURES)
            set(CMAKE_OSX_ARCHITECTURES "arm64;x86_64" CACHE STRING
                "Build fat library for arm64 + x86_64")
        endif()
        message(STATUS "teleproto3: macOS universal binary (${CMAKE_OSX_ARCHITECTURES})")
    else()
        if(NOT CMAKE_OSX_ARCHITECTURES)
            set(CMAKE_OSX_ARCHITECTURES "arm64" CACHE STRING
                "Build arm64-only (mandatory; x86_64 is opt-in)")
        endif()
        message(STATUS "teleproto3: macOS arm64-only (${CMAKE_OSX_ARCHITECTURES})")
    endif()
endif()

set(T3_SANITIZE "" CACHE STRING
    "Comma-separated sanitizers to enable (e.g. 'address,undefined').
     Applied to the library and all test executables.
     Use Clang for best ASan/UBSan diagnostics.")

option(T3_HARDENING
    "Enable production hardening flags: -fstack-protector-strong
     -D_FORTIFY_SOURCE=2 -fno-strict-aliasing -fno-omit-frame-pointer"
    OFF)

# -----------------------------------------------------------------------
# Per-platform CSPRNG backend selection
# -----------------------------------------------------------------------
# Default is 'AUTO' — stories 1-12 (Linux) and 1-13 (macOS) are done.
# AUTO detects the host platform and selects the matching CSPRNG backend.
# Windows backend (1-14) is ready-for-dev; until it lands, Windows builds
# must pass -DT3_CSPRNG_BACKEND=none explicitly if needed for CI.
set(T3_CSPRNG_BACKEND "AUTO" CACHE STRING
    "CSPRNG backend: linux | macos | windows | none | AUTO (host-detect).
     AUTO selects the platform backend based on CMAKE_SYSTEM_NAME.
     'none' returns T3_ERR_RNG unconditionally — use only for CI/portability
     gate builds where entropy is intentionally unavailable.")
set_property(CACHE T3_CSPRNG_BACKEND PROPERTY STRINGS linux macos windows none AUTO)

if(T3_CSPRNG_BACKEND STREQUAL "AUTO")
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        set(T3_CSPRNG_BACKEND_RESOLVED "linux")
    elseif(CMAKE_SYSTEM_NAME STREQUAL "Darwin")
        set(T3_CSPRNG_BACKEND_RESOLVED "macos")
    elseif(CMAKE_SYSTEM_NAME STREQUAL "Windows")
        set(T3_CSPRNG_BACKEND_RESOLVED "windows")
    else()
        message(FATAL_ERROR
            "T3_CSPRNG_BACKEND=AUTO: unrecognised platform '${CMAKE_SYSTEM_NAME}'. "
            "Set T3_CSPRNG_BACKEND explicitly to linux, macos, or windows.")
    endif()
else()
    set(T3_CSPRNG_BACKEND_RESOLVED "${T3_CSPRNG_BACKEND}")
endif()

message(STATUS "teleproto3: CSPRNG backend = ${T3_CSPRNG_BACKEND_RESOLVED}")

# -----------------------------------------------------------------------
# OpenSSL — required for key derivation and AES-256-CTR framing
# (t3_keyderiv.c and t3_obfuscated2.c are stubs in lib v0.1.0;
# find_package is wired now so platform stories do not need to touch CMake)
# -----------------------------------------------------------------------
find_package(OpenSSL 3.0 QUIET)
if(NOT OpenSSL_FOUND)
    if(CMAKE_SYSTEM_NAME STREQUAL "Darwin")
        message(FATAL_ERROR "teleproto3: OpenSSL 3.0 not found. macOS users should run 'brew install openssl@3' and pass -DOPENSSL_ROOT_DIR=$(brew --prefix openssl@3) to CMake.")
    else()
        message(FATAL_ERROR "teleproto3: OpenSSL 3.0 not found. Please install libssl-dev or equivalent.")
    endif()
endif()

# -----------------------------------------------------------------------
# Static library target
# Artefact: libteleproto3.a (anti-pattern §14: NOT liblibteleproto3.a)
# -----------------------------------------------------------------------
add_library(teleproto3 STATIC
    src/t3_abi_version.c
    src/t3_keyderiv.c
    src/t3_obfuscated2.c
    src/t3_retry.c
    src/t3_secret.c
    src/t3_session_header.c
    src/t3_strerror.c
    src/t3_timing.c
    src/t3_url_parser.c
    src/csprng/t3_csprng_${T3_CSPRNG_BACKEND_RESOLVED}.c
)

# -----------------------------------------------------------------------
# Include directories
# PUBLIC  — consumers see lib/include/ (t3.h only; no internal headers)
# PRIVATE — implementation sees lib/include/internal/
# -----------------------------------------------------------------------
target_include_directories(teleproto3
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include/internal
)

# -----------------------------------------------------------------------
# Link libraries
# -----------------------------------------------------------------------
target_link_libraries(teleproto3 PUBLIC OpenSSL::Crypto)

# Task 3 (Story 1-13): Security.framework for SecRandomCopyBytes on macOS.
# Without this flag the linker will fail with:
#   Undefined symbols for architecture arm64: "_SecRandomCopyBytes"
# The flag is gated on APPLE so Linux and Windows builds are unaffected.
if(CMAKE_SYSTEM_NAME STREQUAL "Darwin" AND T3_CSPRNG_BACKEND_RESOLVED STREQUAL "macos")
    target_link_libraries(teleproto3 PUBLIC "-framework Security")
endif()

# -----------------------------------------------------------------------
# Compiler flags
# -----------------------------------------------------------------------
if(MSVC)
    target_compile_options(teleproto3 PRIVATE /W4 /WX)
    # Ensure consumers can include t3.h without importing from a DLL.
    # Static archive only; dllimport/dllexport not needed.
    target_compile_definitions(teleproto3 PUBLIC T3_STATIC_LIB)
else()
    target_compile_options(teleproto3 PRIVATE
        -Wall -Wextra -Wpedantic -Werror -fvisibility=hidden
    )
endif()

# Require C11 with GNU extensions — matches -std=gnu11 used by legacy Makefile.
# CMAKE_C_EXTENSIONS defaults to ON, so c_std_11 → -std=gnu11 on GCC/Clang.
target_compile_features(teleproto3 PRIVATE c_std_11)

# -----------------------------------------------------------------------
# Sanitizer flags (T3_SANITIZE=address,undefined etc.)
# Applied globally so both lib and test executables are instrumented.
# -----------------------------------------------------------------------
if(T3_SANITIZE)
    if(MSVC)
        message(WARNING "T3_SANITIZE is not supported on MSVC; ignoring.")
    else()
        set(_san_flags "-fsanitize=${T3_SANITIZE};-fno-omit-frame-pointer")
        add_compile_options(${_san_flags})
        add_link_options(${_san_flags})
        message(STATUS "teleproto3: sanitizers = ${T3_SANITIZE}")
    endif()
endif()

# -----------------------------------------------------------------------
# Hardening flags (T3_HARDENING=ON)
# -----------------------------------------------------------------------
if(T3_HARDENING)
    if(NOT MSVC)
        add_compile_options(
            -fstack-protector-strong
            -D_FORTIFY_SOURCE=2
            -fno-strict-aliasing
            -fno-omit-frame-pointer
        )
        message(STATUS "teleproto3: hardening flags enabled")
    endif()
endif()

# -----------------------------------------------------------------------
# Test executables (Story 1-12: CSPRNG smoke + KDF vector runner)
# -----------------------------------------------------------------------
if(T3_BUILD_TESTS)
    enable_testing()

    # CSPRNG statistical smoke test — Linux only
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND
       NOT T3_CSPRNG_BACKEND_RESOLVED STREQUAL "none")
        add_executable(test_csprng_linux
            tests/csprng/test_csprng_linux.c
        )
        target_include_directories(test_csprng_linux PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/include
            ${CMAKE_CURRENT_SOURCE_DIR}/include/internal
        )
        target_link_libraries(test_csprng_linux PRIVATE teleproto3)
        target_compile_features(test_csprng_linux PRIVATE c_std_11)
        add_test(NAME csprng_linux COMMAND test_csprng_linux)
    endif()

    # CSPRNG statistical smoke test — macOS (Task 2, Story 1-13)
    if(CMAKE_SYSTEM_NAME STREQUAL "Darwin" AND T3_CSPRNG_BACKEND_RESOLVED STREQUAL "macos")
        add_executable(test_csprng_macos
            tests/csprng/test_csprng_macos.c
        )
        target_include_directories(test_csprng_macos PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/include
            ${CMAKE_CURRENT_SOURCE_DIR}/include/internal
        )
        target_link_libraries(test_csprng_macos PRIVATE teleproto3)
        target_compile_features(test_csprng_macos PRIVATE c_std_11)
        add_test(NAME csprng_macos COMMAND test_csprng_macos)
    endif()

    # KDF vector reference runner (cross-platform; uses OpenSSL directly)
    add_executable(run_kdf_vectors
        tests/vectors/run_kdf_vectors.c
    )
    target_include_directories(run_kdf_vectors PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
    )
    target_link_libraries(run_kdf_vectors PRIVATE OpenSSL::Crypto)
    target_compile_features(run_kdf_vectors PRIVATE c_std_11)
    if(NOT MSVC)
        target_compile_options(run_kdf_vectors PRIVATE
            -Wall -Wextra -Wpedantic -Werror
        )
    endif()

    # CI diff-check: regenerate hashes and compare against committed reference.
    # Registered on Linux AND macOS (Story 1-13 AC #5: macOS must match linux-reference.sha256).
    set(_ref_hash "${CMAKE_CURRENT_SOURCE_DIR}/tests/vectors/linux-reference.sha256")
    set(_kat_file "${CMAKE_CURRENT_SOURCE_DIR}/../conformance/vectors/kdf-kat.txt")
    if(EXISTS "${_ref_hash}" AND EXISTS "${_kat_file}" AND
       (CMAKE_SYSTEM_NAME STREQUAL "Linux" OR CMAKE_SYSTEM_NAME STREQUAL "Darwin"))
        # Name the test by platform so both appear in ctest output.
        if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
            set(_kdf_test_name kdf_vectors_linux)
        else()
            set(_kdf_test_name kdf_vectors_macos)
        endif()
        add_test(
            NAME ${_kdf_test_name}
            COMMAND ${CMAKE_COMMAND}
                -DRUNNER=$<TARGET_FILE:run_kdf_vectors>
                -DKAT_FILE=${_kat_file}
                -DREF_HASH=${_ref_hash}
                -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/vectors/check_kdf_vectors.cmake
        )
    endif()
endif()

# -----------------------------------------------------------------------
# ABI symbol-list guard
# Linux CI invokes tests/abi/symbol_list_check.sh directly after the build
# step (see .github/workflows/lib-portability.yml). No CMake target wires
# the script here — keeping a single canonical invocation site avoids drift
# between the CMake-target path and the shell-script path.
# -----------------------------------------------------------------------
