/*
 * side_channel.h — shared types and declarations for timing-side-channel emission.
 *
 * This is the reference implementation of the Type3 protocol.
 * Normative behaviour is defined in spec/. Where they differ, spec/ wins.
 *
 * Stability: internal to lib/fuzz/; not a public ABI surface.
 */

#ifndef TELEPROTO3_FUZZ_SIDE_CHANNEL_H
#define TELEPROTO3_FUZZ_SIDE_CHANNEL_H

#include <stddef.h>
#include <stdint.h>
#include "t3.h"

/* setup_test_callbacks -- return a callbacks table backed by:
 *   monotonic_ns : CLOCK_MONOTONIC (Linux) / clock_gettime_nsec_np (macOS)
 *   rng          : /dev/urandom reads
 *   log_sink     : no-op
 *   lower/frame  : NULL (parsers under fuzz never call transport)
 *
 * The returned pointer is to a static const struct; do not free.
 * Caller MUST set g_cbs.struct_size = sizeof(t3_callbacks_t) before the
 * first call -- the static initialiser sets it to zero and
 * LLVMFuzzerInitialize patches it at runtime (pre-C11 workaround). */
const t3_callbacks_t *setup_test_callbacks(void);

/* sc_open_log -- open lib/fuzz/side-channel-<PID>.log in append mode.
 * Must be called once from LLVMFuzzerInitialize.
 * Silently ignores double-open (idempotent). */
void sc_open_log(void);

/* sc_emit -- append one tab-separated record to the side-channel log:
 *   <input_len>\t<sha256_hex(data,len)>\t<parse_ns>\t<total_ns>\t<pid>\n
 *
 * parse_ns : nanoseconds spent inside t3_*_parse only.
 * total_ns : parse_ns + t3_silent_close_delay_sample_ns delta.
 *
 * Thread-safety: each worker writes its own PID-keyed file; no locking needed.
 * Timing note: this function is called AFTER t3_silent_close_delay_sample_ns
 * returns -- NEVER inside LLVMFuzzerCustomMutator (that hook runs before
 * execution; real parse latency is not visible there). */
void sc_emit(size_t input_len, const uint8_t *data,
             uint64_t parse_ns, uint64_t total_ns);

#endif /* TELEPROTO3_FUZZ_SIDE_CHANNEL_H */
