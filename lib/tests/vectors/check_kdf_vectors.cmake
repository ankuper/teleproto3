# check_kdf_vectors.cmake — CTest helper: run run_kdf_vectors and diff output
# against linux-reference.sha256.
#
# Variables (passed via -D):
#   RUNNER   — path to the run_kdf_vectors executable
#   KAT_FILE — path to kdf-kat.txt
#   REF_HASH — path to linux-reference.sha256 (committed reference)
#
# Fails (non-zero exit) if output does not match the reference.
# Story 1-12 (AC #5).

if(NOT RUNNER OR NOT KAT_FILE OR NOT REF_HASH)
    message(FATAL_ERROR
        "check_kdf_vectors.cmake: RUNNER, KAT_FILE, and REF_HASH must all be set.")
endif()

# Run the vector runner and capture stdout.
execute_process(
    COMMAND "${RUNNER}" "${KAT_FILE}"
    OUTPUT_VARIABLE runner_output
    ERROR_VARIABLE  runner_error
    RESULT_VARIABLE runner_result
)
if(runner_result)
    message(FATAL_ERROR
        "run_kdf_vectors failed (exit ${runner_result}):\n${runner_error}")
endif()

# Read the committed reference (strip comment lines).
file(STRINGS "${REF_HASH}" ref_lines)
set(ref_data "")
foreach(line IN LISTS ref_lines)
    string(STRIP "${line}" stripped)
    if(NOT stripped MATCHES "^#" AND NOT stripped STREQUAL "")
        string(APPEND ref_data "${stripped}\n")
    endif()
endforeach()

# Strip comment lines from runner output for comparison.
string(REPLACE "\r\n" "\n" runner_output "${runner_output}")
string(REGEX REPLACE "#[^\n]*\n?" "" runner_clean "${runner_output}")

# Normalise both sides (trim trailing whitespace per line).
string(STRIP "${ref_data}" ref_norm)
string(STRIP "${runner_clean}" run_norm)

if(NOT ref_norm STREQUAL run_norm)
    message(FATAL_ERROR
        "KDF vector mismatch!\n"
        "--- expected (linux-reference.sha256) ---\n${ref_norm}\n"
        "--- got (run_kdf_vectors output) ---\n${run_norm}\n"
        "If the KDF implementation changed intentionally, regenerate "
        "linux-reference.sha256 with run_kdf_vectors and commit the update "
        "alongside a lib version bump (lib-v0.2.x).")
endif()

message(STATUS "kdf_vectors_linux: all ${ref_norm} vectors match reference.")
