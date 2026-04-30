/*
 * secret_fuzz.c — libFuzzer harness for t3_secret_parse (story 1-10, AC#1–4, AC#6–9, AC#11).
 *
 * This is the reference implementation of the Type3 protocol.
 * Normative behaviour is defined in spec/. Where they differ, spec/ wins.
 *
 * Stability: internal to lib/fuzz/; not a public ABI surface.
 */

/* Crash mode (make fuzz-crash):
 *   Built with -fsanitize=fuzzer,address,undefined.
 *   No timing emission. Pairs t3_secret_parse with t3_secret_free on T3_OK
 *   to keep the corpus clean of leaks (relevant under ASan).
 *
 * Timing mode (make fuzz-timing):
 *   Built with -fsanitize=fuzzer only, -O2.
 *   Emits <input_len>\t<sha256>\t<parse_ns>\t<total_ns>\t<pid> records.
 *   Optional CustomMutator biases toward bucket-edge lengths for secret format. */

#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdlib.h>

#include "t3.h"
#include "side_channel.h"

static const t3_callbacks_t *g_cb;
static t3_session_t         *g_sess;

/* Minimal valid 24-byte v1 secret (see header_fuzz.c for format rationale). */
static const uint8_t k_min_secret[] = {
    0xee,
    0x01,0x02,0x03,0x04,0x05,0x06,0x07,0x08,
    0x09,0x0a,0x0b,0x0c,0x0d,0x0e,0x0f,0x10,
    '1','.','1','.','1','.','1'
};

int LLVMFuzzerInitialize(int *argc, char ***argv) {
    (void)argc; (void)argv;

    g_cb = setup_test_callbacks();

    t3_secret_t *sec = NULL;
    t3_result_t rc = t3_secret_parse(k_min_secret, sizeof(k_min_secret), &sec);
    if (rc != T3_OK || !sec) {
        g_sess = NULL;
    } else {
        if (t3_session_new(sec, &g_sess) != T3_OK) g_sess = NULL;
        t3_secret_free(sec);
        if (g_sess) {
            if (t3_session_bind_callbacks(g_sess, g_cb) != T3_OK) {
                t3_session_free(g_sess);
                g_sess = NULL;
            }
        }
    }

#ifdef T3_FUZZ_TIMING
    sc_open_log();
#endif
    return 0;
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t len) {
    t3_secret_t *sec = NULL;

#ifdef T3_FUZZ_TIMING
    uint64_t t0 = g_cb->monotonic_ns(g_cb->ctx);
    (void)t3_secret_parse(data, len, &sec);
    uint64_t t1 = g_cb->monotonic_ns(g_cb->ctx);
    if (sec) { t3_secret_free(sec); sec = NULL; }

    /* AC#3: if session setup failed, discard this record entirely. */
    if (!g_sess) return 0;

    uint64_t delay_ns = 0;
    /* PR3 erratum: first arg is t3_session_t*, not t3_callbacks_t*. */
    if (t3_silent_close_delay_sample_ns(g_sess, &delay_ns) != T3_OK) {
        return 0;
    }
    uint64_t t2 = g_cb->monotonic_ns(g_cb->ctx);
    (void)delay_ns;

    /* Emission AFTER t3_silent_close_delay_sample_ns — NOT in CustomMutator. */
    sc_emit(len, data, /*parse_ns=*/t1 - t0, /*total_ns=*/t2 - t0);

#else
    /* Crash mode: parse then free on success (ASan leak guard). */
    t3_result_t rc = t3_secret_parse(data, len, &sec);
    if (rc == T3_OK && sec) {
        t3_secret_free(sec);
    }
#endif

    return 0;
}

/* Optional CustomMutator biasing toward secret-format bucket-edge lengths:
 * 17 (minimum v1), 33, 64, 128, 253, 512.
 * MUST NOT call sc_emit (runs before execution). */
#ifdef T3_FUZZ_TIMING
static const size_t k_target_lens[] = {17, 33, 64, 128, 253, 512};
#define K_N_LENS (sizeof(k_target_lens)/sizeof(k_target_lens[0]))

size_t LLVMFuzzerCustomMutator(uint8_t *data, size_t size,
                                size_t max_size, unsigned int seed) {
    size_t want = k_target_lens[seed % K_N_LENS];
    if (want > max_size) want = max_size;
    if (size < want)  memset(data + size, 0, want - size);
    return want;
}
#endif
