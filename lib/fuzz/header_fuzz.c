/*
 * header_fuzz.c — libFuzzer harness for t3_header_parse (story 1-10, AC#1–4, AC#6–9, AC#11).
 *
 * This is the reference implementation of the Type3 protocol.
 * Normative behaviour is defined in spec/. Where they differ, spec/ wins.
 *
 * Stability: internal to lib/fuzz/; not a public ABI surface.
 */

/* Crash mode (make fuzz-crash):
 *   Built with -fsanitize=fuzzer,address,undefined.
 *   No timing emission (sanitizers perturb timing — measurement invalid).
 *
 * Timing mode (make fuzz-timing):
 *   Built with -fsanitize=fuzzer only, -O2 -fno-sanitize=address,undefined.
 *   Emits <input_len>\t<sha256>\t<parse_ns>\t<total_ns>\t<pid> records.
 *
 * Byte-order round-trip corpus (see Dev Notes §"Routing-fix from 1-6"):
 *   Seed corpus at lib/fuzz/corpus/header_byteorder/ exercises LE flags.
 *   ONLY the first 4 bytes of the fuzz input are consumed by t3_header_parse.
 *   The harness round-trips parse→serialise→parse and asserts byte-identical. */

#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdlib.h>

#include "t3.h"
#include "side_channel.h"

/* g_sess: session bound to g_cb; allocated once in LLVMFuzzerInitialize.
 * PR3 erratum: t3_silent_close_delay_sample_ns takes t3_session_t*, not
 * const t3_callbacks_t*. Reusing g_sess across LLVMFuzzerTestOneInput
 * avoids per-input alloc overhead and is safe (session state is reset
 * by the library for each parse). */
static const t3_callbacks_t *g_cb;
static t3_session_t         *g_sess;

/* Secret used to create the session.  t3_session_new requires a parsed
 * secret; we supply a minimal valid 21-byte v1 secret (16-byte key +
 * 4-byte IPv4 host "1.1.1.1" + implicit path "/"). */
static const uint8_t k_min_secret[] = {
    /* Type3 secret format v1: 0xee marker + 16-byte key + host bytes.
     * Host "1.1.1.1" as 4-byte packed IPv4 is NOT the secret format;
     * instead, secret-format.md stores host as a domain string.
     * Minimal v1 = 0xee | key[16] | "1.1.1.1" ascii (7 bytes) = 24 bytes. */
    0xee,
    0x01,0x02,0x03,0x04,0x05,0x06,0x07,0x08,
    0x09,0x0a,0x0b,0x0c,0x0d,0x0e,0x0f,0x10, /* key */
    '1','.','1','.','1','.','1'              /* host */
};

int LLVMFuzzerInitialize(int *argc, char ***argv) {
    (void)argc; (void)argv;

    g_cb = setup_test_callbacks();

    /* Parse the minimal secret to get a t3_secret_t for session creation. */
    t3_secret_t *sec = NULL;
    t3_result_t rc = t3_secret_parse(k_min_secret, sizeof(k_min_secret), &sec);
    if (rc != T3_OK || !sec) {
        /* No session — timing-mode will skip t3_silent_close_delay_sample_ns. */
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
    /* AC#1 — crash mode: parse any input; sanitizers catch memory errors. */

    uint8_t buf[4] = {0};
    size_t n = len < 4 ? len : 4;
    memcpy(buf, data, n);

    t3_header_t hdr;

#ifdef T3_FUZZ_TIMING
    /* Timing mode: emit (parse_ns, total_ns) per AC#2, AC#4. */
    uint64_t t0 = g_cb->monotonic_ns(g_cb->ctx);
    (void)t3_header_parse(buf, &hdr);
    uint64_t t1 = g_cb->monotonic_ns(g_cb->ctx);

    /* AC#3: if session setup failed, discard this record entirely. */
    if (!g_sess) return 0;

    uint64_t delay_ns = 0;
    /* PR3 erratum: t3_silent_close_delay_sample_ns(t3_session_t*, ...) */
    if (t3_silent_close_delay_sample_ns(g_sess, &delay_ns) != T3_OK) {
        /* Non-T3_OK: discard this record — treat as setup failure. */
        return 0;
    }
    uint64_t t2 = g_cb->monotonic_ns(g_cb->ctx);
    (void)delay_ns; /* delay_ns is implicit in t2-t0 (total_ns) */

    /* Timing emission AFTER t3_silent_close_delay_sample_ns returns.
     * Never inside LLVMFuzzerCustomMutator. */
    sc_emit(len, data, /*parse_ns=*/t1 - t0, /*total_ns=*/t2 - t0);

#else
    /* Crash mode — no timing instrumentation. */
    t3_result_t rc = t3_header_parse(buf, &hdr);

    /* Byte-order round-trip regression (Dev Notes §"Routing-fix from 1-6").
     * If parse succeeded, serialise back and re-parse: output must be
     * byte-identical to the original 4-byte input (LE flags invariant). */
    if (rc == T3_OK) {
        uint8_t rebuf[4] = {0};
        if (t3_header_serialise(&hdr, rebuf) == T3_OK) {
            /* The round-trip assertion: parse(serialise(parse(buf))) == parse(buf).
             * We compare the serialised bytes, not the struct (flags is
             * stored host-order in struct; wire is LE). */
            if (memcmp(buf, rebuf, 4) != 0) {
                /* Byte-order regression: abort to trigger a fuzzer crash report. */
                __builtin_trap();
            }
        }
    }
#endif

    return 0;
}

/* Optional LLVMFuzzerCustomMutator for length-bucket-guided coverage.
 * This hook runs BEFORE execution and MUST NOT call sc_emit.
 * It biases toward lengths that land in secret-format bucket edges. */
#ifdef T3_FUZZ_TIMING
size_t LLVMFuzzerCustomMutator(uint8_t *data, size_t size,
                                size_t max_size, unsigned int seed) {
    /* Bias toward len=4 (fixed header size); no timing emission here. */
    (void)seed;
    /* Always return 4 bytes for the header harness. */
    size_t want = 4;
    if (want > max_size) want = max_size;
    /* Zero-pad or truncate to exactly 4 bytes. */
    if (size < want) memset(data + size, 0, want - size);
    return want;
}
#endif
