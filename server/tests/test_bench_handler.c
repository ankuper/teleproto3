/*
 * test_bench_handler.c — green-phase acceptance tests for Story 1a-2 bench handler.
 *
 * Source: story 1a-2 AC#1–#6 (double gate, sub-mode dispatch, hot-path safety,
 *         runtime default OFF, stats counters).
 * Returns 0 on pass / 1 on fail.
 *
 * BUILD: Compile with -DTELEPROTO3_BENCH to enable the test body.
 *   Without the flag the test prints SKIP and exits 0 (non-gating).
 *   Link against bench-handler.o.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#ifdef TELEPROTO3_BENCH

#include "net/bench-handler.h"

/* ------------------------------------------------------------------ */
/* Helpers                                                             */
/* ------------------------------------------------------------------ */

static int g_failures = 0;

#define EXPECT_EQ(label, got, want) do { \
    if ((int)(got) != (int)(want)) { \
        fprintf(stderr, "FAIL [%s]: got %d, want %d\n", (label), (int)(got), (int)(want)); \
        g_failures++; \
    } else { \
        printf("PASS [%s]\n", (label)); \
    } \
} while (0)

#define EXPECT_EQ_U64(label, got, want) do { \
    if ((uint64_t)(got) != (uint64_t)(want)) { \
        fprintf(stderr, "FAIL [%s]: got %llu, want %llu\n", (label), \
                (unsigned long long)(got), (unsigned long long)(want)); \
        g_failures++; \
    } else { \
        printf("PASS [%s]\n", (label)); \
    } \
} while (0)

#define EXPECT_MEM_EQ(label, got, want, len) do { \
    if (memcmp((got), (want), (len)) != 0) { \
        fprintf(stderr, "FAIL [%s]: memory mismatch (%d bytes)\n", (label), (int)(len)); \
        g_failures++; \
    } else { \
        printf("PASS [%s]\n", (label)); \
    } \
} while (0)

static bench_conn_t *make_conn(void) {
    bench_conn_t *c = calloc(1, sizeof(*c));
    if (!c) { perror("calloc"); exit(1); }
    return c;
}

/* ------------------------------------------------------------------ */
/* Test 1: AC#1 — double gate dispatch (both ON → init succeeds)      */
/* ------------------------------------------------------------------ */
static void test_dispatch_double_gate_on(void) {
    g_bench_handler_enabled = 1;
    bench_conn_t *c = make_conn();
    int rc = bench_handler_init(c);
    EXPECT_EQ("1a2-UNIT-001: double gate ON -> init returns 0", rc, 0);
    free(c);
}

/* ------------------------------------------------------------------ */
/* Test 2: AC#1/AC#5 — runtime gate OFF → init rejected               */
/* ------------------------------------------------------------------ */
static void test_dispatch_runtime_gate_off(void) {
    g_bench_handler_enabled = 0;
    bench_conn_t *c = make_conn();
    int rc = bench_handler_init(c);
    EXPECT_EQ("1a2-UNIT-002: runtime gate OFF -> init returns -1", rc, -1);
    free(c);
}

/* ------------------------------------------------------------------ */
/* Test 3: AC#2 — sub-mode SINK (0x01): read+discard, no response     */
/* ------------------------------------------------------------------ */
static void test_submode_sink(void) {
    g_bench_handler_enabled = 1;
    bench_conn_t *c = make_conn();
    bench_handler_init(c);

    uint8_t payload[33];
    payload[0] = BENCH_MODE_SINK;
    memset(payload + 1, 0xAB, 32);

    int rc = bench_handler_recv(c, payload, sizeof(payload));
    EXPECT_EQ("1a2-UNIT-003a: sink recv returns 32 (consumed)", rc, 32);
    EXPECT_EQ("1a2-UNIT-003b: sink produces no output", c->out_len, 0);

    free(c);
}

/* ------------------------------------------------------------------ */
/* Test 4: AC#2 — sub-mode ECHO (0x02): echoes bytes back identically */
/* ------------------------------------------------------------------ */
static void test_submode_echo(void) {
    g_bench_handler_enabled = 1;
    bench_conn_t *c = make_conn();
    bench_handler_init(c);

    uint8_t payload[17];
    payload[0] = BENCH_MODE_ECHO;
    for (int i = 0; i < 16; i++) {
        payload[1 + i] = (uint8_t)(i * 7 + 3);
    }

    int rc = bench_handler_recv(c, payload, sizeof(payload));
    EXPECT_EQ("1a2-UNIT-004a: echo recv returns 16 (payload bytes)", rc, 16);
    EXPECT_EQ("1a2-UNIT-004b: echo output length is 16", c->out_len, 16);
    EXPECT_MEM_EQ("1a2-UNIT-004c: echo output matches input",
                  c->out_buf, payload + 1, 16);

    free(c);
}

/* ------------------------------------------------------------------ */
/* Test 5: AC#2 — sub-mode SOURCE (0x03): reads 4-byte LE length N,   */
/*          emits N CSPRNG bytes                                       */
/* ------------------------------------------------------------------ */
static void test_submode_source(void) {
    g_bench_handler_enabled = 1;
    bench_conn_t *c = make_conn();
    bench_handler_init(c);

    uint8_t payload[5];
    payload[0] = BENCH_MODE_SOURCE;
    uint32_t requested = 256;
    payload[1] = (uint8_t)(requested & 0xff);
    payload[2] = (uint8_t)((requested >> 8) & 0xff);
    payload[3] = (uint8_t)((requested >> 16) & 0xff);
    payload[4] = (uint8_t)((requested >> 24) & 0xff);

    int rc = bench_handler_recv(c, payload, sizeof(payload));
    /* C5: SOURCE completes in one call → BENCH_RC_SOURCE_DONE, not the byte count */
    EXPECT_EQ("1a2-UNIT-005a: source recv returns BENCH_RC_SOURCE_DONE", rc, BENCH_RC_SOURCE_DONE);
    EXPECT_EQ("1a2-UNIT-005b: source output length is 256", c->out_len, 256);

    int all_zero = 1;
    for (int i = 0; i < 256 && all_zero; i++) {
        if (c->out_buf[i] != 0) all_zero = 0;
    }
    EXPECT_EQ("1a2-UNIT-005c: source output not all zeros", all_zero, 0);

    free(c);
}

/* ------------------------------------------------------------------ */
/* Test 6: AC#2 — invalid sub-mode (0xFF) → WS close 1003             */
/* ------------------------------------------------------------------ */
static void test_submode_invalid(void) {
    g_bench_handler_enabled = 1;
    bench_conn_t *c = make_conn();
    bench_handler_init(c);

    uint8_t payload[2];
    payload[0] = 0xFF;
    payload[1] = 0x00;

    int rc = bench_handler_recv(c, payload, sizeof(payload));
    EXPECT_EQ("1a2-UNIT-006: invalid mode 0xFF -> returns -1003", rc, -1003);

    free(c);
}

/* ------------------------------------------------------------------ */
/* Test 7: AC#6 — stats counters reflect correct byte counts           */
/* ------------------------------------------------------------------ */
static void test_stats_counters(void) {
    g_bench_handler_enabled = 1;

    bench_stats_t before = bench_handler_get_stats();

    /* SINK: 32 bytes */
    {
        bench_conn_t *c = make_conn();
        bench_handler_init(c);
        uint8_t payload[33];
        payload[0] = BENCH_MODE_SINK;
        memset(payload + 1, 0xCC, 32);
        bench_handler_recv(c, payload, sizeof(payload));
        free(c);
    }

    /* ECHO: 20 bytes */
    {
        bench_conn_t *c = make_conn();
        bench_handler_init(c);
        uint8_t payload[21];
        payload[0] = BENCH_MODE_ECHO;
        memset(payload + 1, 0xDD, 20);
        bench_handler_recv(c, payload, sizeof(payload));
        free(c);
    }

    /* SOURCE: 64 bytes */
    {
        bench_conn_t *c = make_conn();
        bench_handler_init(c);
        uint8_t payload[5];
        payload[0] = BENCH_MODE_SOURCE;
        uint32_t req = 64;
        payload[1] = (uint8_t)(req & 0xff);
        payload[2] = (uint8_t)((req >> 8) & 0xff);
        payload[3] = (uint8_t)((req >> 16) & 0xff);
        payload[4] = (uint8_t)((req >> 24) & 0xff);
        bench_handler_recv(c, payload, sizeof(payload));
        free(c);
    }

    bench_stats_t after = bench_handler_get_stats();

    EXPECT_EQ_U64("1a2-UNIT-007a: sink_bytes delta == 32",
                  after.sink_bytes   - before.sink_bytes,   32);
    EXPECT_EQ_U64("1a2-UNIT-007b: echo_bytes delta == 20",
                  after.echo_bytes   - before.echo_bytes,   20);
    EXPECT_EQ_U64("1a2-UNIT-007c: source_bytes delta == 64",
                  after.source_bytes - before.source_bytes, 64);
}

/* ------------------------------------------------------------------ */
/* Test 8: AC#6 — stats stay flat when handler disabled                */
/* ------------------------------------------------------------------ */
static void test_stats_zero_when_disabled(void) {
    bench_stats_t before = bench_handler_get_stats();

    g_bench_handler_enabled = 0;

    bench_conn_t *c = make_conn();
    int rc = bench_handler_init(c);
    EXPECT_EQ("1a2-UNIT-008a: init rejected when disabled", rc, -1);

    uint8_t payload[17];
    payload[0] = BENCH_MODE_ECHO;
    memset(payload + 1, 0xEE, 16);
    rc = bench_handler_recv(c, payload, sizeof(payload));
    if (rc >= 0) {
        fprintf(stderr, "FAIL [1a2-UNIT-008b]: recv should fail when disabled, got %d\n", rc);
        g_failures++;
    } else {
        printf("PASS [1a2-UNIT-008b]: recv fails when disabled (rc=%d)\n", rc);
    }

    bench_stats_t after = bench_handler_get_stats();

    EXPECT_EQ_U64("1a2-UNIT-008c: sink_bytes unchanged",
                  after.sink_bytes, before.sink_bytes);
    EXPECT_EQ_U64("1a2-UNIT-008d: echo_bytes unchanged",
                  after.echo_bytes, before.echo_bytes);
    EXPECT_EQ_U64("1a2-UNIT-008e: source_bytes unchanged",
                  after.source_bytes, before.source_bytes);

    free(c);
}

/* ------------------------------------------------------------------ */
/* Test 9: SOURCE — 4-byte length split across two recv calls          */
/* Exercises the back-pressure / partial-input handling (AC #3).       */
/* ------------------------------------------------------------------ */
static void test_source_split_length(void) {
    g_bench_handler_enabled = 1;
    bench_conn_t *c = make_conn();
    bench_handler_init(c);

    uint8_t first[3];
    first[0] = BENCH_MODE_SOURCE;
    first[1] = 0x80;  /* length LE: 128 */
    first[2] = 0x00;
    int rc = bench_handler_recv(c, first, sizeof(first));
    EXPECT_EQ("1a2-UNIT-009a: partial length returns 0 (waiting)", rc, 0);
    EXPECT_EQ("1a2-UNIT-009b: no output before length complete", c->out_len, 0);

    uint8_t rest[2];
    rest[0] = 0x00;
    rest[1] = 0x00;
    rc = bench_handler_recv(c, rest, sizeof(rest));
    /* C5: SOURCE completes in this call → BENCH_RC_SOURCE_DONE */
    EXPECT_EQ("1a2-UNIT-009c: rest of length -> BENCH_RC_SOURCE_DONE", rc, BENCH_RC_SOURCE_DONE);
    EXPECT_EQ("1a2-UNIT-009d: source output length is 128", c->out_len, 128);

    free(c);
}

/* ------------------------------------------------------------------ */
/* Test 10: C6 — SOURCE continuation with len=0 when N > BENCH_OUT_CAP */
/* Verifies that calling bench_handler_recv(c, NULL, 0) continues       */
/* emitting when source_remaining > 0 after an initial partial emit.   */
/* ------------------------------------------------------------------ */
static void test_source_continuation(void) {
    g_bench_handler_enabled = 1;
    bench_conn_t *c = make_conn();
    bench_handler_init(c);

    /* Request N = 2 * BENCH_OUT_CAP = 8192 bytes. */
    uint32_t requested = BENCH_OUT_CAP * 2;
    uint8_t payload[5];
    payload[0] = BENCH_MODE_SOURCE;
    payload[1] = (uint8_t)(requested & 0xff);
    payload[2] = (uint8_t)((requested >> 8) & 0xff);
    payload[3] = (uint8_t)((requested >> 16) & 0xff);
    payload[4] = (uint8_t)((requested >> 24) & 0xff);

    /* First call: emits BENCH_OUT_CAP bytes, source_remaining = BENCH_OUT_CAP.
     * Returns bytes_emitted (BENCH_OUT_CAP), NOT BENCH_RC_SOURCE_DONE. */
    int rc = bench_handler_recv(c, payload, sizeof(payload));
    EXPECT_EQ("1a2-UNIT-010a: first partial emit returns BENCH_OUT_CAP", rc, BENCH_OUT_CAP);
    EXPECT_EQ("1a2-UNIT-010b: out_len == BENCH_OUT_CAP after first emit",
              c->out_len, BENCH_OUT_CAP);

    /* Simulate drain: reset out_len (drain loop flushes out_buf between calls). */
    c->out_len = 0;

    /* Continuation call with len=0: emits remaining BENCH_OUT_CAP bytes. */
    rc = bench_handler_recv(c, NULL, 0);
    EXPECT_EQ("1a2-UNIT-010c: continuation call returns BENCH_RC_SOURCE_DONE", rc, BENCH_RC_SOURCE_DONE);
    EXPECT_EQ("1a2-UNIT-010d: out_len == BENCH_OUT_CAP after continuation",
              c->out_len, BENCH_OUT_CAP);

    free(c);
}

/* ------------------------------------------------------------------ */
/* Test 11 (P1 R2): SOURCE N=0 boundary — must close immediately       */
/* with BENCH_RC_SOURCE_DONE rather than leaving a zombie session.    */
/* ------------------------------------------------------------------ */
static void test_source_zero_length(void) {
    g_bench_handler_enabled = 1;
    bench_conn_t *c = make_conn();
    bench_handler_init(c);

    /* SOURCE with N=0 (4 zero bytes after the mode byte). */
    uint8_t payload[5] = { BENCH_MODE_SOURCE, 0x00, 0x00, 0x00, 0x00 };
    int rc = bench_handler_recv(c, payload, sizeof(payload));
    EXPECT_EQ("1a2-UNIT-011a: SOURCE N=0 -> BENCH_RC_SOURCE_DONE", rc, BENCH_RC_SOURCE_DONE);
    EXPECT_EQ("1a2-UNIT-011b: SOURCE N=0 -> out_len == 0", c->out_len, 0);

    free(c);
}

/* ------------------------------------------------------------------ */
/* Test 12 (P12 R2 / D2): SOURCE N > BENCH_SOURCE_MAX must reject     */
/* with -1003 before any CSPRNG emission. AC #3 hot-path safety.      */
/* ------------------------------------------------------------------ */
static void test_source_max_n_reject(void) {
    g_bench_handler_enabled = 1;
    bench_conn_t *c = make_conn();
    bench_handler_init(c);

    /* SOURCE with N = BENCH_SOURCE_MAX + 1 (just over the cap). */
    uint32_t over = BENCH_SOURCE_MAX + 1u;
    uint8_t payload[5];
    payload[0] = BENCH_MODE_SOURCE;
    payload[1] = (uint8_t)(over & 0xff);
    payload[2] = (uint8_t)((over >> 8) & 0xff);
    payload[3] = (uint8_t)((over >> 16) & 0xff);
    payload[4] = (uint8_t)((over >> 24) & 0xff);

    int rc = bench_handler_recv(c, payload, sizeof(payload));
    EXPECT_EQ("1a2-UNIT-012a: SOURCE N>MAX -> -1003", rc, -1003);
    EXPECT_EQ("1a2-UNIT-012b: SOURCE N>MAX -> no output emitted", c->out_len, 0);

    free(c);
}

/* ------------------------------------------------------------------ */
/* main                                                                */
/* ------------------------------------------------------------------ */
int main(void) {
    printf("=== test_bench_handler: Story 1a-2 acceptance tests ===\n\n");

    test_dispatch_double_gate_on();
    test_dispatch_runtime_gate_off();
    test_submode_sink();
    test_submode_echo();
    test_submode_source();
    test_submode_invalid();
    test_stats_counters();
    test_stats_zero_when_disabled();
    test_source_split_length();
    test_source_continuation();
    test_source_zero_length();
    test_source_max_n_reject();

    printf("\n");
    if (g_failures > 0) {
        fprintf(stderr, "=== RESULT: FAIL (%d failures) ===\n", g_failures);
        return 1;
    }
    printf("=== RESULT: PASS ===\n");
    return 0;
}

#else /* !TELEPROTO3_BENCH */

int main(void) {
    printf("=== test_bench_handler: SKIP (TELEPROTO3_BENCH not defined) ===\n");
    return 0;
}

#endif /* TELEPROTO3_BENCH */
