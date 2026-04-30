/*
 * dispatch_test.c — red-phase acceptance tests for the Type3 server dispatcher.
 *
 * Not subject to banner-discipline (tests/, not src/).
 * Source: story 1.8 AC#1,#3,#5,#6,#7 + dispatch_test.c Task 6.1.
 * Returns 0 on pass / 1 on fail.
 *
 * TDD RED PHASE: will FAIL until net-type3-dispatch.c is fully implemented
 * per story 1.8 Tasks 1.1–1.5 and the subtree is imported (Task 0).
 *
 * BUILD NOTE: Link against libteleproto3.a + server/net/net-type3-dispatch.o
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

/* Forward declarations (from net-type3-dispatch.h, included here symbolically).
 * The actual header lives at teleproto3/server/net/net-type3-dispatch.h.
 * At scaffold time we reproduce only what we need. */

typedef enum {
    TYPE3_DISPATCH_PASSTHROUGH   = 0,
    TYPE3_DISPATCH_ACCEPT        = 1,
    TYPE3_DISPATCH_DROP_SILENT   = 2,
} type3_dispatch_outcome_t;

/* Opaque stub — struct connection is upstream-defined (after Task 0). */
struct connection;

/* Declared in net-type3-dispatch.h: */
extern type3_dispatch_outcome_t type3_dispatch_on_crypto_init(struct connection *c);

/* ------------------------------------------------------------------ */
/* Helpers                                                              */
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

/* ------------------------------------------------------------------ */
/* Fixture: build a mock connection that contains a pre-set header     */
/* ------------------------------------------------------------------ */

/* We cannot include net-connections.h until the subtree is imported.  */
/* Instead, we test via the conformance vectors from unit.json — the  */
/* dispatcher fixture reads the header bytes from c->in.               */
/*                                                                     */
/* At red-phase, we compile this file as a stub:                       */
/*   - If the upstream struct is not yet available, tests print SKIP.  */
/*   - Once Task 0 lands, the includes are replaced with real headers. */

#ifndef T3_DISPATCH_UPSTREAM_AVAILABLE

static int test_dispatch_accept_header(void) {
    fprintf(stderr, "SKIP [1.8-UNIT-001]: upstream struct not yet available (Task 0 pending)\n");
    return 0; /* non-gating at this scaffold phase */
}
static int test_dispatch_drop_silent(void) {
    fprintf(stderr, "SKIP [1.8-UNIT-002]: upstream struct not yet available\n");
    return 0;
}
static int test_dispatch_passthrough(void) {
    fprintf(stderr, "SKIP [1.8-UNIT-003]: upstream struct not yet available\n");
    return 0;
}

#else

/*
 * When T3_DISPATCH_UPSTREAM_AVAILABLE is defined (after Task 0 + Task 1.7),
 * include real headers and drive actual struct connections.
 */
#include "net-type3-dispatch.h"
#include "net-connections.h"  /* upstream: struct connection */
#include "t3.h"

static struct connection *make_conn_with_header(const uint8_t hdr[4], int is_type3_ws) {
    /* Allocate a zeroed connection; set ws_state and crypto fields
     * per story 1.8 Task 2.1 insertion-point contract. */
    struct connection *c = calloc(1, sizeof(*c));
    if (!c) { perror("calloc"); exit(1); }
    /* Copy 4-byte session header into c->in buffer. */
    memcpy(c->in.data, hdr, 4);
    c->in.data_len = 4;
    c->ws_state = is_type3_ws ? WS_ST_ACTIVE : 0;
    c->crypto   = NULL; /* pre-crypto handshake */
    return c;
}

static int test_dispatch_accept_header(void) {
    /* A well-formed Type3 WS session header. */
    /* command_type=0x01 (Type3), version=0x01, flags=0x0000 */
    uint8_t hdr[4] = {0x01, 0x01, 0x00, 0x00};
    struct connection *c = make_conn_with_header(hdr, 1 /*is_ws*/);
    type3_dispatch_outcome_t r = type3_dispatch_on_crypto_init(c);
    EXPECT_EQ("1.8-UNIT-001: ok header -> ACCEPT", r, TYPE3_DISPATCH_ACCEPT);
    free(c);
    return 0;
}

static int test_dispatch_drop_silent(void) {
    /* Malformed header: reserved-bit pattern per spec/wire-format.md §3. */
    uint8_t hdr[4] = {0xFF, 0xFF, 0xFF, 0xFF};
    struct connection *c = make_conn_with_header(hdr, 1);
    type3_dispatch_outcome_t r = type3_dispatch_on_crypto_init(c);
    EXPECT_EQ("1.8-UNIT-002: malformed header -> DROP_SILENT", r, TYPE3_DISPATCH_DROP_SILENT);
    free(c);
    return 0;
}

static int test_dispatch_passthrough(void) {
    /* Non-WS connection (Type1/Type2 path): ws_state != WS_ST_ACTIVE. */
    uint8_t hdr[4] = {0x01, 0x01, 0x00, 0x00};
    struct connection *c = make_conn_with_header(hdr, 0 /*not ws*/);
    type3_dispatch_outcome_t r = type3_dispatch_on_crypto_init(c);
    EXPECT_EQ("1.8-UNIT-003: non-WS -> PASSTHROUGH", r, TYPE3_DISPATCH_PASSTHROUGH);
    free(c);
    return 0;
}

#endif /* T3_DISPATCH_UPSTREAM_AVAILABLE */

/* ------------------------------------------------------------------ */
/* Test: silent-close delay comes from lib API, not rand()             */
/* 1.8-UNIT-006                                                        */
/* ------------------------------------------------------------------ */
static int test_silent_close_uses_lib_api(void) {
    /*
     * This test audits the SOURCE of the delay. We cannot call
     * type3_dispatch_silent_close() directly and observe the delay at
     * scaffold time, but we CAN assert that:
     *   1. `rand() % 151` or `rand() % 150` is NOT present in the source.
     *   2. `t3_silent_close_delay_sample_ns` IS referenced.
     *
     * Use a grep on the source file as a compile-time surrogate.
     */
    int bad = system("grep -q 'rand()' ../src/net/net-type3-dispatch.c 2>/dev/null");
    if (bad == 0) {
        fprintf(stderr, "FAIL [1.8-UNIT-006]: rand() usage found in dispatcher — must use t3_silent_close_delay_sample_ns\n");
        g_failures++;
        return 1;
    }
    int good = system("grep -q 't3_silent_close_delay_sample_ns' ../src/net/net-type3-dispatch.c 2>/dev/null");
    if (good != 0) {
        fprintf(stderr, "FAIL [1.8-UNIT-006]: t3_silent_close_delay_sample_ns not found in dispatcher\n");
        g_failures++;
        return 1;
    }
    printf("PASS [1.8-UNIT-006]: dispatcher uses t3_silent_close_delay_sample_ns\n");
    return 0;
}

/* ------------------------------------------------------------------ */
/* Test: insertion-point has exactly BEGIN/END sentinel (8 lines)      */
/* 1.8-UNIT-005                                                        */
/* ------------------------------------------------------------------ */
static int test_insertion_point_sentinel(void) {
    int begin_count = system(
        "grep -c 'BEGIN AR-S2 dispatch hook' "
        "../src/net/net-tcp-rpc-ext-server.c >/dev/null 2>&1");
    int end_count = system(
        "grep -c 'END AR-S2 dispatch hook' "
        "../src/net/net-tcp-rpc-ext-server.c >/dev/null 2>&1");
    /* grep -c returns 0 if ≥1 match. */
    if (begin_count == 0 && end_count == 0) {
        printf("PASS [1.8-UNIT-005]: insertion-point sentinels present\n");
        return 0;
    }
    fprintf(stderr, "FAIL [1.8-UNIT-005]: insertion-point sentinels missing in net-tcp-rpc-ext-server.c\n");
    g_failures++;
    return 1;
}

/* ------------------------------------------------------------------ */
/* main                                                                */
/* ------------------------------------------------------------------ */
int main(void) {
    printf("=== dispatch_test: RED-PHASE acceptance scaffold ===\n");

    test_dispatch_accept_header();
    test_dispatch_drop_silent();
    test_dispatch_passthrough();
    test_silent_close_uses_lib_api();
    test_insertion_point_sentinel();

    if (g_failures > 0) {
        fprintf(stderr, "\n=== RESULT: FAIL (%d failures) ===\n", g_failures);
        return 1;
    }
    printf("\n=== RESULT: PASS ===\n");
    return 0;
}
