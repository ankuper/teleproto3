/*
 * test_bench_handler.c — red-phase acceptance tests for Story 1a-2 bench handler.
 *
 * TDD RED PHASE: will FAIL until bench-handler.c is implemented
 *
 * Not subject to banner-discipline (tests/, not src/).
 * Source: story 1a-2 AC#1–#6 (double gate, sub-mode dispatch, hot-path safety,
 *         runtime default OFF, stats counters).
 * Returns 0 on pass / 1 on fail.
 *
 * BUILD NOTE: Compile with -DTELEPROTO3_BENCH to enable the test body.
 *   Without the flag the test prints SKIP and exits 0 (non-gating).
 *   Link against bench-handler.o (once implemented).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#ifdef TELEPROTO3_BENCH

/* ------------------------------------------------------------------ */
/* Forward declarations — API surface expected from bench-handler.h   */
/* These will be satisfied once bench-handler.c is implemented.       */
/* ------------------------------------------------------------------ */

/* Opaque upstream struct — we mock it locally for unit tests. */
struct connection;

/* Sub-mode constants (spec/wire-format.md §3.1, amendment W-003) */
#define BENCH_MODE_SINK   0x01
#define BENCH_MODE_ECHO   0x02
#define BENCH_MODE_SOURCE 0x03

/* State per connection */
typedef struct {
    uint8_t   mode;             /* 0x01=SINK, 0x02=ECHO, 0x03=SOURCE */
    uint64_t  bytes_processed;
    uint32_t  source_remaining;
} bench_handler_state_t;

/* Init: called from dispatcher when command_type==0x04 */
extern int bench_handler_init(struct connection *c);

/* Recv callback: parse mode byte on first call, dispatch by mode.
 * Returns: 0 on success, negative on error.
 * For ECHO: writes echoed bytes into the connection's output buffer.
 * For SOURCE: writes N random bytes into the connection's output buffer.
 * For errors: returns -1003 to signal WS close 1003. */
extern int bench_handler_recv(struct connection *c, void *data, int len);

/* Stats */
typedef struct {
    uint64_t sink_bytes;
    uint64_t echo_bytes;
    uint64_t source_bytes;
} bench_stats_t;

extern bench_stats_t bench_handler_get_stats(void);

/* Runtime gate — default 0 (OFF). Set to 1 by --enable-bench-handler. */
extern int g_bench_handler_enabled;

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

/* ------------------------------------------------------------------ */
/* Mock connection                                                     */
/*                                                                     */
/* We cannot include the real upstream struct connection until the      */
/* subtree is fully wired (Story 1.8 Task 0). Instead we allocate a   */
/* block large enough to hold the bench_handler_state_t at the offset  */
/* the implementation will use, plus a simple output capture buffer.   */
/*                                                                     */
/* The bench handler is expected to use:                               */
/*   c->bench_state  (bench_handler_state_t)                           */
/*   c->out_buf / c->out_len for echo/source output                   */
/*                                                                     */
/* We define a minimal mock that provides these fields.                */
/* ------------------------------------------------------------------ */

#define MOCK_OUT_CAP 4096

struct connection {
    bench_handler_state_t bench_state;
    uint8_t  out_buf[MOCK_OUT_CAP];
    int      out_len;
    /* Padding to survive if real struct is larger — we only pass
     * pointers to bench_handler_* which should only touch the above. */
    uint8_t  _pad[512];
};

static struct connection *make_mock_conn(void) {
    struct connection *c = calloc(1, sizeof(*c));
    if (!c) { perror("calloc"); exit(1); }
    return c;
}

/* ------------------------------------------------------------------ */
/* Test 1: AC#1 — double gate dispatch (both ON → init succeeds)      */
/* ------------------------------------------------------------------ */
static int test_dispatch_double_gate_on(void) {
    g_bench_handler_enabled = 1;  /* runtime gate ON */
    struct connection *c = make_mock_conn();

    int rc = bench_handler_init(c);
    EXPECT_EQ("1a2-UNIT-001: double gate ON -> init returns 0", rc, 0);

    free(c);
    return 0;
}

/* ------------------------------------------------------------------ */
/* Test 2: AC#1/AC#5 — runtime gate OFF → init rejected               */
/* ------------------------------------------------------------------ */
static int test_dispatch_runtime_gate_off(void) {
    g_bench_handler_enabled = 0;  /* runtime gate OFF */
    struct connection *c = make_mock_conn();

    int rc = bench_handler_init(c);
    EXPECT_EQ("1a2-UNIT-002: runtime gate OFF -> init returns -1", rc, -1);

    free(c);
    return 0;
}

/* ------------------------------------------------------------------ */
/* Test 3: AC#2 — sub-mode SINK (0x01): read+discard, no response     */
/* ------------------------------------------------------------------ */
static int test_submode_sink(void) {
    g_bench_handler_enabled = 1;
    struct connection *c = make_mock_conn();
    bench_handler_init(c);

    /* First byte = mode, followed by 32 bytes of payload. */
    uint8_t payload[33];
    payload[0] = BENCH_MODE_SINK;
    memset(payload + 1, 0xAB, 32);

    int rc = bench_handler_recv(c, payload, sizeof(payload));
    EXPECT_EQ("1a2-UNIT-003a: sink recv returns 0", rc, 0);
    /* Sink produces no output. */
    EXPECT_EQ("1a2-UNIT-003b: sink produces no output", c->out_len, 0);

    free(c);
    return 0;
}

/* ------------------------------------------------------------------ */
/* Test 4: AC#2 — sub-mode ECHO (0x02): echoes bytes back identically */
/* ------------------------------------------------------------------ */
static int test_submode_echo(void) {
    g_bench_handler_enabled = 1;
    struct connection *c = make_mock_conn();
    bench_handler_init(c);

    /* First byte = mode, followed by 16 bytes of known data. */
    uint8_t payload[17];
    payload[0] = BENCH_MODE_ECHO;
    for (int i = 0; i < 16; i++) {
        payload[1 + i] = (uint8_t)(i * 7 + 3);  /* deterministic pattern */
    }

    int rc = bench_handler_recv(c, payload, sizeof(payload));
    EXPECT_EQ("1a2-UNIT-004a: echo recv returns 16 (payload bytes)", rc, 16);
    EXPECT_EQ("1a2-UNIT-004b: echo output length is 16", c->out_len, 16);
    EXPECT_MEM_EQ("1a2-UNIT-004c: echo output matches input",
                  c->out_buf, payload + 1, 16);

    free(c);
    return 0;
}

/* ------------------------------------------------------------------ */
/* Test 5: AC#2 — sub-mode SOURCE (0x03): reads 4-byte LE length N,   */
/*          emits N random bytes                                       */
/* ------------------------------------------------------------------ */
static int test_submode_source(void) {
    g_bench_handler_enabled = 1;
    struct connection *c = make_mock_conn();
    bench_handler_init(c);

    /* First byte = mode, then 4-byte LE length = 256. */
    uint8_t payload[5];
    payload[0] = BENCH_MODE_SOURCE;
    uint32_t requested = 256;
    memcpy(payload + 1, &requested, 4);  /* LE on LE host (Linux-only) */

    int rc = bench_handler_recv(c, payload, sizeof(payload));
    EXPECT_EQ("1a2-UNIT-005a: source recv returns 256", rc, 256);
    EXPECT_EQ("1a2-UNIT-005b: source output length is 256", c->out_len, 256);

    /* We can't predict the random content, but we can verify it's not
     * all zeros (extremely unlikely for 256 random bytes). */
    int all_zero = 1;
    for (int i = 0; i < 256 && all_zero; i++) {
        if (c->out_buf[i] != 0) all_zero = 0;
    }
    EXPECT_EQ("1a2-UNIT-005c: source output not all zeros", all_zero, 0);

    free(c);
    return 0;
}

/* ------------------------------------------------------------------ */
/* Test 6: AC#2 — invalid sub-mode (0xFF) → WS close 1003             */
/* ------------------------------------------------------------------ */
static int test_submode_invalid(void) {
    g_bench_handler_enabled = 1;
    struct connection *c = make_mock_conn();
    bench_handler_init(c);

    uint8_t payload[2];
    payload[0] = 0xFF;  /* invalid sub-mode */
    payload[1] = 0x00;  /* dummy byte */

    int rc = bench_handler_recv(c, payload, sizeof(payload));
    /* -1003 signals the caller to issue WS close with status 1003
     * ("Unsupported Data" per RFC 6455 §7.4.1). */
    EXPECT_EQ("1a2-UNIT-006: invalid mode 0xFF -> returns -1003", rc, -1003);

    free(c);
    return 0;
}

/* ------------------------------------------------------------------ */
/* Test 7: AC#6 — stats counters reflect correct byte counts           */
/* ------------------------------------------------------------------ */
static int test_stats_counters(void) {
    g_bench_handler_enabled = 1;

    /* --- SINK: 32 bytes --- */
    {
        struct connection *c = make_mock_conn();
        bench_handler_init(c);
        uint8_t payload[33];
        payload[0] = BENCH_MODE_SINK;
        memset(payload + 1, 0xCC, 32);
        bench_handler_recv(c, payload, sizeof(payload));
        free(c);
    }

    /* --- ECHO: 20 bytes --- */
    {
        struct connection *c = make_mock_conn();
        bench_handler_init(c);
        uint8_t payload[21];
        payload[0] = BENCH_MODE_ECHO;
        memset(payload + 1, 0xDD, 20);
        bench_handler_recv(c, payload, sizeof(payload));
        free(c);
    }

    /* --- SOURCE: 64 bytes --- */
    {
        struct connection *c = make_mock_conn();
        bench_handler_init(c);
        uint8_t payload[5];
        payload[0] = BENCH_MODE_SOURCE;
        uint32_t req = 64;
        memcpy(payload + 1, &req, 4);
        bench_handler_recv(c, payload, sizeof(payload));
        free(c);
    }

    bench_stats_t stats = bench_handler_get_stats();

    /* Note: these assertions assume the stats counters started from zero
     * at the top of this test process. If previous tests already bumped
     * counters, these values reflect cumulative totals. We check >=
     * the expected minimum from this test's operations. For a clean
     * test run where tests execute sequentially in a single process,
     * the expected values include bytes from earlier test functions too.
     *
     * Minimum bytes contributed by THIS function:
     *   sink:   32
     *   echo:   20
     *   source: 64
     *
     * Bytes from earlier tests (test_submode_sink, echo, source):
     *   sink:   32 (test 3)
     *   echo:   16 (test 4)
     *   source: 256 (test 5)
     *
     * Total expected:
     *   sink:   64
     *   echo:   36
     *   source: 320
     */
    int sink_ok   = (stats.sink_bytes   >= 64);
    int echo_ok   = (stats.echo_bytes   >= 36);
    int source_ok = (stats.source_bytes >= 320);

    EXPECT_EQ("1a2-UNIT-007a: sink_bytes >= 64",   sink_ok,   1);
    EXPECT_EQ("1a2-UNIT-007b: echo_bytes >= 36",   echo_ok,   1);
    EXPECT_EQ("1a2-UNIT-007c: source_bytes >= 320", source_ok, 1);

    return 0;
}

/* ------------------------------------------------------------------ */
/* Test 8: AC#6 — stats zero when handler disabled                     */
/*                                                                     */
/* When the runtime gate is OFF, no operations should succeed, so      */
/* no bytes should be counted. We test by disabling the handler,       */
/* attempting operations, and checking that stats did not grow.        */
/* ------------------------------------------------------------------ */
static int test_stats_zero_when_disabled(void) {
    /* Snapshot current stats (from prior tests). */
    bench_stats_t before = bench_handler_get_stats();

    g_bench_handler_enabled = 0;  /* runtime gate OFF */

    /* Attempt init — should fail. */
    struct connection *c = make_mock_conn();
    int rc = bench_handler_init(c);
    EXPECT_EQ("1a2-UNIT-008a: init rejected when disabled", rc, -1);

    /* Attempt recv anyway — should also fail / be a no-op. */
    uint8_t payload[17];
    payload[0] = BENCH_MODE_ECHO;
    memset(payload + 1, 0xEE, 16);
    rc = bench_handler_recv(c, payload, sizeof(payload));
    /* Handler should reject if init was never successful.
     * Exact error code is implementation-defined; we just check
     * it's not a success (0 or positive). */
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
    return 0;
}

/* ------------------------------------------------------------------ */
/* main                                                                */
/* ------------------------------------------------------------------ */
int main(void) {
    printf("=== test_bench_handler: RED-PHASE acceptance scaffold (Story 1a-2) ===\n\n");

    test_dispatch_double_gate_on();
    test_dispatch_runtime_gate_off();
    test_submode_sink();
    test_submode_echo();
    test_submode_source();
    test_submode_invalid();
    test_stats_counters();
    test_stats_zero_when_disabled();

    printf("\n");
    if (g_failures > 0) {
        fprintf(stderr, "=== RESULT: FAIL (%d failures) ===\n", g_failures);
        return 1;
    }
    printf("=== RESULT: PASS ===\n");
    return 0;
}

#else /* !TELEPROTO3_BENCH */

/* Compile gate is OFF — test is non-gating. */
int main(void) {
    printf("=== test_bench_handler: SKIP (TELEPROTO3_BENCH not defined) ===\n");
    return 0;
}

#endif /* TELEPROTO3_BENCH */
