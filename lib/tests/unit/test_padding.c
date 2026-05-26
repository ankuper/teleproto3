/*
 * test_padding.c — unit tests for padding and splitting API.
 * Not subject to banner-discipline (tests/, not src/).
 * Source: story 11-3 Task 5.
 * Returns 0 on pass / 1 on fail.
 */

#include "t3.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int stub_rng_counter(void *ctx, uint8_t *buf, size_t len) {
    uint32_t *counter = (uint32_t *)ctx;
    for (size_t i = 0; i < len; i++) {
        /* Simple LCG style step to generate varying non-zero bytes */
        *counter = (*counter * 1103515245u + 12345u);
        buf[i] = (uint8_t)((*counter / 65536u) % 256u);
    }
    return 0;
}

static int64_t stub_ls(void *ctx, const uint8_t *b, size_t l) { (void)ctx; (void)b; (void)l; return 0; }
static int64_t stub_lr(void *ctx, uint8_t *b, size_t l) { (void)ctx; (void)b; (void)l; return 0; }
static int64_t stub_fs(void *ctx, const uint8_t *b, size_t l, int f) { (void)ctx; (void)b; (void)l; (void)f; return 0; }
static int64_t stub_fr(void *ctx, uint8_t *b, size_t c, int *o) { (void)ctx; (void)b; (void)c; (void)o; return 0; }
static uint64_t stub_clock(void *ctx) { (void)ctx; return 0; }

static t3_session_t *make_session(uint32_t *rng_state) {
    uint8_t sb[18];
    memset(sb, 0, 18);
    sb[0] = 0xFF;
    sb[17] = 'x';
    t3_secret_t *sec = NULL;
    if (t3_secret_parse(sb, 18, &sec) != T3_OK) return NULL;
    t3_session_t *sess = NULL;
    if (t3_session_new(sec, &sess) != T3_OK) {
        t3_secret_free(sec);
        return NULL;
    }
    t3_callbacks_t cb;
    memset(&cb, 0, sizeof(cb));
    cb.struct_size = sizeof(cb);
    cb.lower_send = stub_ls;
    cb.lower_recv = stub_lr;
    cb.frame_send = stub_fs;
    cb.frame_recv = stub_fr;
    cb.rng = stub_rng_counter;
    cb.monotonic_ns = stub_clock;
    cb.ctx = rng_state;

    if (t3_session_bind_callbacks(sess, &cb) != T3_OK) {
        t3_session_free(sess);
        t3_secret_free(sec);
        return NULL;
    }
    t3_secret_free(sec);
    return sess;
}

static int test_detect(void) {
    printf("[test_detect] starting...\n");
    if (t3_padding_detect(T3_PADDING_MARKER) != 1) {
        fprintf(stderr, "[test_detect] FAIL: failed to detect 0xFE\n");
        return 1;
    }
    for (int i = 0; i < 256; i++) {
        if (i != T3_PADDING_MARKER) {
            if (t3_padding_detect((uint8_t)i) != 0) {
                fprintf(stderr, "[test_detect] FAIL: false positive detection for byte 0x%02X\n", i);
                return 1;
            }
        }
    }
    printf("[test_detect] PASS\n");
    return 0;
}

static int test_generate_happy(void) {
    printf("[test_generate_happy] starting...\n");
    uint32_t rng_state = 42;
    t3_session_t *sess = make_session(&rng_state);
    if (!sess) {
        fprintf(stderr, "[test_generate_happy] FAIL: make_session failed\n");
        return 1;
    }

    uint8_t buf[256];
    size_t out_len = 0;
    t3_result_t rc;

    /* Generate multiple times, verify constraints, verify random fill */
    int has_non_zero = 0;
    for (int i = 0; i < 20; i++) {
        memset(buf, 0, sizeof(buf));
        rc = t3_padding_generate(sess, buf, 32, 128, &out_len);
        if (rc != T3_OK) {
            fprintf(stderr, "[test_generate_happy] FAIL: rc=%d\n", rc);
            t3_session_free(sess);
            return 1;
        }
        if (out_len < 32 || out_len > 128) {
            fprintf(stderr, "[test_generate_happy] FAIL: length %zu out of bounds [32, 128]\n", out_len);
            t3_session_free(sess);
            return 1;
        }
        if (buf[0] != T3_PADDING_MARKER) {
            fprintf(stderr, "[test_generate_happy] FAIL: missing padding marker at index 0\n");
            t3_session_free(sess);
            return 1;
        }
        /* Verify non-zero fill */
        for (size_t j = 1; j < out_len; j++) {
            if (buf[j] != 0) {
                has_non_zero = 1;
            }
        }
    }
    if (!has_non_zero) {
        fprintf(stderr, "[test_generate_happy] FAIL: padding bytes were all zeroes\n");
        t3_session_free(sess);
        return 1;
    }

    t3_session_free(sess);
    printf("[test_generate_happy] PASS\n");
    return 0;
}

static int test_generate_edge(void) {
    printf("[test_generate_edge] starting...\n");
    uint32_t rng_state = 100;
    t3_session_t *sess = make_session(&rng_state);
    if (!sess) return 1;

    uint8_t buf[10];
    size_t out_len = 0;
    memset(buf, 0, sizeof(buf));

    /* min_len == max_len == 1 */
    t3_result_t rc = t3_padding_generate(sess, buf, 1, 1, &out_len);
    if (rc != T3_OK) {
        fprintf(stderr, "[test_generate_edge] FAIL: rc=%d\n", rc);
        t3_session_free(sess);
        return 1;
    }
    if (out_len != 1) {
        fprintf(stderr, "[test_generate_edge] FAIL: out_len=%zu\n", out_len);
        t3_session_free(sess);
        return 1;
    }
    if (buf[0] != T3_PADDING_MARKER) {
        fprintf(stderr, "[test_generate_edge] FAIL: marker missing\n");
        t3_session_free(sess);
        return 1;
    }

    t3_session_free(sess);
    printf("[test_generate_edge] PASS\n");
    return 0;
}

static int test_generate_errors(void) {
    printf("[test_generate_errors] starting...\n");
    uint32_t rng_state = 999;
    t3_session_t *sess = make_session(&rng_state);
    if (!sess) return 1;

    uint8_t buf[100];
    size_t out_len = 0;

    /* NULL guards */
    if (t3_padding_generate(NULL, buf, 10, 20, &out_len) != T3_ERR_INVALID_ARG) return 1;
    if (t3_padding_generate(sess, NULL, 10, 20, &out_len) != T3_ERR_INVALID_ARG) return 1;
    if (t3_padding_generate(sess, buf, 10, 20, NULL) != T3_ERR_INVALID_ARG) return 1;

    /* Parameter constraints */
    if (t3_padding_generate(sess, buf, 0, 20, &out_len) != T3_ERR_INVALID_ARG) return 1;
    if (t3_padding_generate(sess, buf, 20, 10, &out_len) != T3_ERR_INVALID_ARG) return 1;
    if (t3_padding_generate(sess, buf, 10, 65536, &out_len) != T3_ERR_INVALID_ARG) return 1;

    /* Callbacks not bound */
    t3_session_t *unbound = NULL;
    uint8_t sb[18]; memset(sb, 0, 18); sb[0] = 0xFF; sb[17] = 'x';
    t3_secret_t *sec = NULL;
    if (t3_secret_parse(sb, 18, &sec) == T3_OK) {
        if (t3_session_new(sec, &unbound) == T3_OK) {
            if (t3_padding_generate(unbound, buf, 10, 20, &out_len) != T3_ERR_INVALID_ARG) {
                t3_session_free(unbound);
                t3_secret_free(sec);
                t3_session_free(sess);
                return 1;
            }
            t3_session_free(unbound);
        }
        t3_secret_free(sec);
    }

    t3_session_free(sess);
    printf("[test_generate_errors] PASS\n");
    return 0;
}

static int test_split_happy(void) {
    printf("[test_split_happy] starting...\n");
    uint32_t rng_state = 12345;
    t3_session_t *sess = make_session(&rng_state);
    if (!sess) return 1;

    size_t plan[100];
    size_t out_count = 0;

    for (int run = 0; run < 50; run++) {
        memset(plan, 0, sizeof(plan));
        t3_result_t rc = t3_split_plan(sess, 1000, 100, 400, plan, 10, &out_count);
        if (rc != T3_OK) {
            fprintf(stderr, "[test_split_happy] FAIL: rc=%d\n", rc);
            t3_session_free(sess);
            return 1;
        }
        if (out_count == 0 || out_count > 10) {
            fprintf(stderr, "[test_split_happy] FAIL: out_count=%zu\n", out_count);
            t3_session_free(sess);
            return 1;
        }

        size_t sum = 0;
        for (size_t i = 0; i < out_count; i++) {
            if (plan[i] < 100 || plan[i] > 400) {
                fprintf(stderr, "[test_split_happy] FAIL: plan[%zu]=%zu\n", i, plan[i]);
                t3_session_free(sess);
                return 1;
            }
            sum += plan[i];
        }
        if (sum != 1000) {
            fprintf(stderr, "[test_split_happy] FAIL: sum=%zu, expected 1000\n", sum);
            t3_session_free(sess);
            return 1;
        }
    }

    t3_session_free(sess);
    printf("[test_split_happy] PASS\n");
    return 0;
}

static int test_split_edge(void) {
    printf("[test_split_edge] starting...\n");
    uint32_t rng_state = 999;
    t3_session_t *sess = make_session(&rng_state);
    if (!sess) return 1;

    size_t plan[5];
    size_t out_count = 0;

    /* Single chunk */
    t3_result_t rc = t3_split_plan(sess, 100, 100, 100, plan, 1, &out_count);
    if (rc != T3_OK || out_count != 1 || plan[0] != 100) {
        fprintf(stderr, "[test_split_edge] FAIL single-chunk\n");
        t3_session_free(sess);
        return 1;
    }

    t3_session_free(sess);
    printf("[test_split_edge] PASS\n");
    return 0;
}

static int test_split_buf_too_small(void) {
    printf("[test_split_buf_too_small] starting...\n");
    uint32_t rng_state = 777;
    t3_session_t *sess = make_session(&rng_state);
    if (!sess) return 1;

    size_t plan[10];
    size_t out_count = 0;

    /* Max cover is 5 * 100 = 500. total_len is 1000. Should return T3_ERR_BUF_TOO_SMALL */
    t3_result_t rc = t3_split_plan(sess, 1000, 10, 100, plan, 5, &out_count);
    if (rc != T3_ERR_BUF_TOO_SMALL) {
        fprintf(stderr, "[test_split_buf_too_small] FAIL: expected T3_ERR_BUF_TOO_SMALL, got %d\n", rc);
        t3_session_free(sess);
        return 1;
    }

    t3_session_free(sess);
    printf("[test_split_buf_too_small] PASS\n");
    return 0;
}

static int test_split_errors(void) {
    printf("[test_split_errors] starting...\n");
    uint32_t rng_state = 111;
    t3_session_t *sess = make_session(&rng_state);
    if (!sess) return 1;

    size_t plan[10];
    size_t out_count = 0;

    /* NULL guards */
    if (t3_split_plan(NULL, 100, 10, 20, plan, 10, &out_count) != T3_ERR_INVALID_ARG) return 1;
    if (t3_split_plan(sess, 100, 10, 20, NULL, 10, &out_count) != T3_ERR_INVALID_ARG) return 1;
    if (t3_split_plan(sess, 100, 10, 20, plan, 10, NULL) != T3_ERR_INVALID_ARG) return 1;

    /* Constraints */
    if (t3_split_plan(sess, 0, 10, 20, plan, 10, &out_count) != T3_ERR_INVALID_ARG) return 1;
    if (t3_split_plan(sess, 100, 0, 20, plan, 10, &out_count) != T3_ERR_INVALID_ARG) return 1;
    if (t3_split_plan(sess, 100, 20, 10, plan, 10, &out_count) != T3_ERR_INVALID_ARG) return 1;
    if (t3_split_plan(sess, 100, 10, 20, plan, 0, &out_count) != T3_ERR_INVALID_ARG) return 1;
    if (t3_split_plan(sess, 50, 100, 200, plan, 10, &out_count) != T3_ERR_INVALID_ARG) return 1;

    t3_session_free(sess);
    printf("[test_split_errors] PASS\n");
    return 0;
}

static int test_split_tight_bounds(void) {
    /* Exercises the clamping edge case identified in code review F-1:
     * total_len=5, min_chunk=3, max_chunk=4, max_chunks=2.
     * Feasibility: 2*4=8 >= 5. Both chunks must be in [3,4] and sum to 5.
     * Only valid split: (3,2) is invalid since 2<min_chunk; so result must be
     * (3+2) — but wait: the only valid partition is one chunk of 3 and one
     * of 2 — except 2 < min_chunk=3. Actually the only valid partition of 5
     * into exactly 2 chunks in [3,4] is impossible (3+3=6>5, 3+2 invalid).
     * So the function MUST return T3_ERR_INTERNAL (feasibility over-approved
     * this case — the guard catches it).
     * Separately: test a legitimately tight-but-valid case:
     * total_len=7, min_chunk=3, max_chunk=4, max_chunks=2 → (3,4) or (4,3). */
    printf("[test_split_tight_bounds] starting...\n");
    uint32_t rng_state = 55555;
    t3_session_t *sess = make_session(&rng_state);
    if (!sess) return 1;

    size_t plan[5];
    size_t out_count = 0;
    t3_result_t rc;

    /* Case A: total_len=5, min=3, max=4, max_chunks=2
     * Feasibility passes (2*4=8>=5) but no valid 2-chunk partition exists
     * in [3,4] summing to 5 — the H<L guard must fire → T3_ERR_INTERNAL. */
    rc = t3_split_plan(sess, 5, 3, 4, plan, 2, &out_count);
    if (rc != T3_ERR_INTERNAL) {
        fprintf(stderr, "[test_split_tight_bounds] FAIL case-A: expected T3_ERR_INTERNAL, got %d\n", rc);
        t3_session_free(sess);
        return 1;
    }

    /* Case B: total_len=7, min=3, max=4, max_chunks=2
     * Valid partitions: (3,4) and (4,3). Must return T3_OK, sum=7. */
    for (int run = 0; run < 20; run++) {
        memset(plan, 0, sizeof(plan));
        rc = t3_split_plan(sess, 7, 3, 4, plan, 2, &out_count);
        if (rc != T3_OK) {
            fprintf(stderr, "[test_split_tight_bounds] FAIL case-B run %d: rc=%d\n", run, rc);
            t3_session_free(sess);
            return 1;
        }
        if (out_count < 1 || out_count > 2) {
            fprintf(stderr, "[test_split_tight_bounds] FAIL case-B: out_count=%zu\n", out_count);
            t3_session_free(sess);
            return 1;
        }
        size_t sum = 0;
        for (size_t i = 0; i < out_count; i++) {
            if (plan[i] < 3 || plan[i] > 4) {
                fprintf(stderr, "[test_split_tight_bounds] FAIL case-B: plan[%zu]=%zu out of [3,4]\n", i, plan[i]);
                t3_session_free(sess);
                return 1;
            }
            sum += plan[i];
        }
        if (sum != 7) {
            fprintf(stderr, "[test_split_tight_bounds] FAIL case-B run %d: sum=%zu\n", run, sum);
            t3_session_free(sess);
            return 1;
        }
    }

    t3_session_free(sess);
    printf("[test_split_tight_bounds] PASS\n");
    return 0;
}

int main(void) {
    int rc = 0;
    rc |= test_detect();
    rc |= test_generate_happy();
    rc |= test_generate_edge();
    rc |= test_generate_errors();
    rc |= test_split_happy();
    rc |= test_split_edge();
    rc |= test_split_buf_too_small();
    rc |= test_split_errors();
    rc |= test_split_tight_bounds();
    return rc;
}
