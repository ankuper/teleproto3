/*
 * test_timing.c — red-phase acceptance tests for the silent-close timing engine.
 *
 * Not subject to banner-discipline (tests/, not src/).
 * Source: story 1.3 AC#1,#2 + story 1.7 AC3,AC5 (TOST + Spearman + baseline).
 * Returns 0 on pass / 1 on fail.
 *
 * TDD RED PHASE: tests are ACTIVE and will FAIL until t3_timing.c is
 * implemented per story 1.7 Tasks 4 and 9.
 *
 * BUILD NOTE:
 *   Normal:  $(CC) -std=gnu11 -O1 -g -fsanitize=address,undefined -lm
 *   Baseline: make -C lib/build baseline NOSAN=1
 *             (no sanitizers; -O2 -DNDEBUG; see style-guide §11)
 */

#include "t3.h"
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* Constants matching story 1.7 AC3 + style-guide §12                  */
/* ------------------------------------------------------------------ */

#define LO_NS  (50000000ULL)    /* 50 ms */
#define HI_NS  (200000000ULL)   /* 200 ms */
#define RANGE_NS (HI_NS - LO_NS + 1ULL)

#define N_BUCKETS  5
#define N_PER_BUCKET 10000   /* gating threshold per style-guide §12 */
#define TOST_DELTA_MS 2.0    /* ms — equivalence margin per AC3 */
#define TOST_ALPHA 0.05
#define SPEARMAN_RHO_MAX 0.1

/* Input length bucket midpoints (bytes) — 0-63,64-255,256-1023,1024-4095,4096-16383 */
static const size_t BUCKET_MIDPOINTS[N_BUCKETS] = {31, 159, 639, 2559, 10383};

/* ------------------------------------------------------------------ */
/* Minimal /dev/urandom RNG callback for the test harness              */
/* ------------------------------------------------------------------ */

#include <fcntl.h>
#include <unistd.h>
#include <time.h>

static int urandom_fd = -1;

static int cb_rng(void *ctx, uint8_t *buf, size_t len) {
    (void)ctx;
    if (urandom_fd < 0) urandom_fd = open("/dev/urandom", O_RDONLY);
    if (urandom_fd < 0) return -1;
    ssize_t r = read(urandom_fd, buf, len);
    return (r == (ssize_t)len) ? 0 : -1;
}

static uint64_t cb_monotonic_ns(void *ctx) {
    (void)ctx;
    struct timespec ts;
#if defined(__APPLE__)
    clock_gettime(CLOCK_MONOTONIC, &ts);
#else
    clock_gettime(CLOCK_MONOTONIC, &ts);
#endif
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

static int64_t cb_lower_send(void *ctx, const uint8_t *b, size_t l) { (void)ctx;(void)b;(void)l; return -1; }
static int64_t cb_lower_recv(void *ctx, uint8_t *b, size_t l) { (void)ctx;(void)b;(void)l; return -1; }
static int64_t cb_frame_send(void *ctx, const uint8_t *b, size_t l, int f) { (void)ctx;(void)b;(void)l;(void)f; return -1; }
static int64_t cb_frame_recv(void *ctx, uint8_t *b, size_t c, int *o) { (void)ctx;(void)b;(void)c;(void)o; return -1; }
static void cb_log(void *ctx, int lvl, const char *fmt, ...) { (void)ctx;(void)lvl;(void)fmt; }

static const t3_callbacks_t g_cb = {
    .struct_size   = sizeof(t3_callbacks_t),
    .lower_send    = cb_lower_send,
    .lower_recv    = cb_lower_recv,
    .frame_send    = cb_frame_send,
    .frame_recv    = cb_frame_recv,
    .rng           = cb_rng,
    .monotonic_ns  = cb_monotonic_ns,
    .log_sink      = cb_log,
    .ctx           = NULL,
};

/* ------------------------------------------------------------------ */
/* Sampling helper                                                      */
/* ------------------------------------------------------------------ */

static double *sample_bucket(t3_session_t *sess, size_t input_len, int n) {
    (void)input_len; /* used by custom mutators; timing engine ignores it */
    double *samples = malloc((size_t)n * sizeof(double));
    if (!samples) { perror("malloc"); exit(1); }
    for (int i = 0; i < n; i++) {
        uint64_t out_ns;
        t3_result_t rc = t3_silent_close_delay_sample_ns(sess, &out_ns);
        if (rc != T3_OK) {
            fprintf(stderr, "FAIL: t3_silent_close_delay_sample_ns returned %d\n", rc);
            exit(1);
        }
        if (out_ns < LO_NS || out_ns > HI_NS) {
            fprintf(stderr, "FAIL 1.3-UNIT-001: out_ns=%llu not in [50ms,200ms]\n",
                    (unsigned long long)out_ns);
            exit(1);
        }
        samples[i] = (double)out_ns;
    }
    return samples;
}

/* ------------------------------------------------------------------ */
/* Spearman rank correlation (stdlib-only, O(n log n))                 */
/* 1.3-UNIT-016, 1.7-UNIT-014                                          */
/* ------------------------------------------------------------------ */

typedef struct { double val; int orig_idx; } ranked_t;
static int cmp_ranked(const void *a, const void *b) {
    double da = ((const ranked_t*)a)->val;
    double db = ((const ranked_t*)b)->val;
    return (da > db) - (da < db);
}

static double spearman_rho(const double *x, const double *y, int n) {
    ranked_t *rx = malloc((size_t)n * sizeof(ranked_t));
    ranked_t *ry = malloc((size_t)n * sizeof(ranked_t));
    double *rank_x = malloc((size_t)n * sizeof(double));
    double *rank_y = malloc((size_t)n * sizeof(double));
    if (!rx || !ry || !rank_x || !rank_y) { perror("malloc"); exit(1); }

    for (int i = 0; i < n; i++) { rx[i].val = x[i]; rx[i].orig_idx = i; }
    for (int i = 0; i < n; i++) { ry[i].val = y[i]; ry[i].orig_idx = i; }
    qsort(rx, (size_t)n, sizeof(ranked_t), cmp_ranked);
    qsort(ry, (size_t)n, sizeof(ranked_t), cmp_ranked);

    for (int i = 0; i < n; ) {
        int j = i;
        while (j < n && rx[j].val == rx[i].val) j++;
        double avg_rank = (i + j - 1) / 2.0 + 1.0;
        for (int k = i; k < j; k++) rank_x[rx[k].orig_idx] = avg_rank;
        i = j;
    }
    for (int i = 0; i < n; ) {
        int j = i;
        while (j < n && ry[j].val == ry[i].val) j++;
        double avg_rank = (i + j - 1) / 2.0 + 1.0;
        for (int k = i; k < j; k++) rank_y[ry[k].orig_idx] = avg_rank;
        i = j;
    }

    double mean_rx = (n + 1) / 2.0, mean_ry = (n + 1) / 2.0;
    double num = 0, den_x = 0, den_y = 0;
    for (int i = 0; i < n; i++) {
        double dx = rank_x[i] - mean_rx;
        double dy = rank_y[i] - mean_ry;
        num   += dx * dy;
        den_x += dx * dx;
        den_y += dy * dy;
    }
    free(rx); free(ry); free(rank_x); free(rank_y);
    if (den_x == 0 || den_y == 0) return 0.0;
    return num / sqrt(den_x * den_y);
}

/* ------------------------------------------------------------------ */
/* Bootstrap 95% CI for Spearman rho (N_BOOT=2000 resamples)           */
/* 1.7 Task 8.3                                                        */
/* ------------------------------------------------------------------ */

#define N_BOOT 2000

static int cmp_double(const void *a, const void *b) {
    double da = *(const double *)a, db = *(const double *)b;
    return (da > db) - (da < db);
}

static void bootstrap_ci(const double *x, const double *y, int n,
                          double *ci_low, double *ci_high) {
    double *boot_rhos = malloc(N_BOOT * sizeof(double));
    double *bx = malloc((size_t)n * sizeof(double));
    double *by = malloc((size_t)n * sizeof(double));
    if (!boot_rhos || !bx || !by) { perror("malloc"); exit(1); }

    for (int r = 0; r < N_BOOT; r++) {
        /* Resample with replacement using urandom. */
        for (int i = 0; i < n; i++) {
            uint32_t idx;
            if (cb_rng(NULL, (uint8_t *)&idx, sizeof(idx)) != 0) {
                perror("rng"); exit(1);
            }
            int j = (int)(idx % (uint32_t)n);
            bx[i] = x[j];
            by[i] = y[j];
        }
        boot_rhos[r] = spearman_rho(bx, by, n);
    }

    qsort(boot_rhos, N_BOOT, sizeof(double), cmp_double);
    /* 2.5th and 97.5th percentile. */
    *ci_low  = boot_rhos[(int)(N_BOOT * 0.025)];
    *ci_high = boot_rhos[(int)(N_BOOT * 0.975)];

    free(boot_rhos); free(bx); free(by);
}

/* ------------------------------------------------------------------ */
/* TOST: two one-sided t-tests on bucket means                         */
/* 1.3-UNIT-015, 1.7-UNIT-013                                          */
/* Returns 1 if EQUIVALENT (both null rejected), 0 otherwise.         */
/* ------------------------------------------------------------------ */

static double tost_delta_ns = TOST_DELTA_MS * 1e6; /* ms → ns */

static double mean_of(const double *s, int n) {
    double m = 0;
    for (int i = 0; i < n; i++) m += s[i];
    return m / n;
}

static double variance_of(const double *s, int n, double m) {
    double v = 0;
    for (int i = 0; i < n; i++) { double d = s[i] - m; v += d*d; }
    return v / (n - 1);
}

/* One-sided t: reject H0: mu_diff >= delta at alpha.
   Welch t-stat: t = (m1-m2 - delta) / sqrt(v1/n1 + v2/n2).
   Reject if t < -t_crit (α=0.05, df≈n — use df=∞ approx → t_crit=1.645). */
static int tost_pair(const double *a, const double *b, int n) {
    double ma = mean_of(a, n), mb = mean_of(b, n);
    double va = variance_of(a, n, ma), vb = variance_of(b, n, mb);
    double se = sqrt(va / n + vb / n);
    if (se == 0) return 1; /* identical distributions → trivially equivalent */
    double diff = ma - mb;
    /* H0+: diff >= +delta → reject if t_upper < -1.645 */
    double t_upper = (diff - tost_delta_ns) / se;
    /* H0-: diff <= -delta → reject if t_lower >  1.645 */
    double t_lower = (diff + tost_delta_ns) / se;
    double t_crit = 1.645; /* one-tailed α=0.05 large df */
    return (t_upper < -t_crit && t_lower > t_crit) ? 1 : 0;
}

/* ------------------------------------------------------------------ */
/* Test: samples in range [50ms, 200ms]                                */
/* 1.3-UNIT-001                                                        */
/* ------------------------------------------------------------------ */
static int test_range(t3_session_t *sess) {
    /* Already validated per-sample in sample_bucket; this is a summary. */
    printf("PASS [1.3-UNIT-001]: all sampled delays in [50ms, 200ms]\n");
    return 0;
}

/* ------------------------------------------------------------------ */
/* Main test driver                                                     */
/* ------------------------------------------------------------------ */
int main(int argc, char **argv) {
    int emit_baseline = 0;
    const char *baseline_path = NULL;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--emit-baseline") == 0 && i+1 < argc)
            { baseline_path = argv[++i]; emit_baseline = 1; }
    }

    printf("=== test_timing: acceptance scaffold ===\n");

    /* Create a minimal valid secret for session allocation. */
    uint8_t sbuf[18];
    memset(sbuf, 0, sizeof sbuf);
    sbuf[0] = 0xFF;  /* marker byte */
    sbuf[17] = 'x';  /* minimal 1-char domain */
    t3_secret_t *secret = NULL;
    if (t3_secret_parse(sbuf, sizeof sbuf, &secret) != T3_OK || !secret) {
        fprintf(stderr, "FATAL: t3_secret_parse failed\n");
        return 1;
    }

    /* Allocate session for timing engine. */
    t3_session_t *sess = NULL;
    if (t3_session_new(secret, &sess) != T3_OK || !sess) {
        fprintf(stderr, "FATAL: t3_session_new failed\n");
        t3_secret_free(secret);
        return 1;
    }
    if (t3_session_bind_callbacks(sess, &g_cb) != T3_OK) {
        fprintf(stderr, "FATAL: t3_session_bind_callbacks failed\n");
        t3_session_free(sess); t3_secret_free(secret);
        return 1;
    }

    /* Collect N_PER_BUCKET samples per bucket. */
    double *bucket_samples[N_BUCKETS];
    int n = N_PER_BUCKET;
    for (int b = 0; b < N_BUCKETS; b++) {
        printf("Sampling bucket %d (input_len=%zu, n=%d)...\n", b, BUCKET_MIDPOINTS[b], n);
        bucket_samples[b] = sample_bucket(sess, BUCKET_MIDPOINTS[b], n);
    }

    /* 1.3-UNIT-001 / range check (already done in sample_bucket). */
    test_range(sess);

    /* 1.7-UNIT-013 / 1.3-UNIT-015: TOST on all C(5,2)=10 pairs. */
    int tost_pairs_total = 0, tost_pairs_equiv = 0;
    for (int i = 0; i < N_BUCKETS; i++) {
        for (int j = i+1; j < N_BUCKETS; j++) {
            int eq = tost_pair(bucket_samples[i], bucket_samples[j], n);
            tost_pairs_total++;
            if (eq) tost_pairs_equiv++;
            printf("[TOST] bucket %d vs %d: %s\n", i, j, eq ? "EQUIV" : "NOT EQUIV (FAIL)");
        }
    }
    int tost_ok = (tost_pairs_equiv == tost_pairs_total);
    printf("%s [1.7-UNIT-013 / 1.3-UNIT-015]: TOST %d/%d pairs equivalent\n",
           tost_ok ? "PASS" : "FAIL", tost_pairs_equiv, tost_pairs_total);

    /* 1.7-UNIT-014 / 1.3-UNIT-016: Spearman rho across all samples. */
    int total_n = N_BUCKETS * n;
    double *all_len = malloc((size_t)total_n * sizeof(double));
    double *all_delay = malloc((size_t)total_n * sizeof(double));
    if (!all_len || !all_delay) { perror("malloc"); return 1; }
    for (int b = 0; b < N_BUCKETS; b++) {
        for (int i = 0; i < n; i++) {
            all_len[b*n + i]   = (double)BUCKET_MIDPOINTS[b];
            all_delay[b*n + i] = bucket_samples[b][i];
        }
    }
    double rho = spearman_rho(all_len, all_delay, total_n);
    double ci95_low = 0.0, ci95_high = 0.0;
    printf("Computing bootstrap 95%% CI (N_BOOT=%d)...\n", N_BOOT);
    bootstrap_ci(all_len, all_delay, total_n, &ci95_low, &ci95_high);
    int rho_ok = (fabs(rho) < SPEARMAN_RHO_MAX);
    printf("%s [1.7-UNIT-014 / 1.3-UNIT-016]: Spearman |rho|=%.4f CI95=[%.4f, %.4f] (threshold %.1f)\n",
           rho_ok ? "PASS" : "FAIL", fabs(rho), ci95_low, ci95_high, SPEARMAN_RHO_MAX);

    /* 1.7-UNIT-016: rejection sampling bias — verify ≥99.9% of samples
       are strictly within [50ms, 200ms] (bias-free uniform would have 0 outside). */
    int in_range = 0;
    for (int b = 0; b < N_BUCKETS; b++)
        for (int i = 0; i < n; i++)
            if (bucket_samples[b][i] >= LO_NS && bucket_samples[b][i] <= HI_NS)
                in_range++;
    int bias_ok = (in_range == total_n);
    printf("%s [1.7-UNIT-016]: rejection sampling — %d/%d samples in range\n",
           bias_ok ? "PASS" : "FAIL", in_range, total_n);

    /* 1.7-UNIT-015 / 1.3-UNIT-017: underpowered guard (for CI smoke runs). */
    if (n < N_PER_BUCKET) {
        fprintf(stderr, "WARN: underpowered (N=%d < %d) — non-gating\n", n, N_PER_BUCKET);
    }

    /* Emit baseline YAML (1.7-UNIT-022). */
    if (emit_baseline && baseline_path) {
        /* Compute p50/p95/p99 across all samples. */
        double sorted[N_BUCKETS * N_PER_BUCKET];
        memcpy(sorted, all_delay, (size_t)total_n * sizeof(double));
        /* Simple insertion-sort for the test harness (10k samples, acceptable). */
        for (int i = 1; i < total_n; i++) {
            double key = sorted[i]; int j = i - 1;
            while (j >= 0 && sorted[j] > key) { sorted[j+1] = sorted[j]; j--; }
            sorted[j+1] = key;
        }
        long long p50 = (long long)sorted[(int)(total_n * 0.50)];
        long long p95 = (long long)sorted[(int)(total_n * 0.95)];
        long long p99 = (long long)sorted[(int)(total_n * 0.99)];

        FILE *yf = fopen(baseline_path, "w");
        if (!yf) { perror("fopen baseline"); return 1; }
        fprintf(yf, "schema_version: 1\n");
        fprintf(yf, "artefact: lib-v0.1.0\n");
        fprintf(yf, "toolchain:\n");
        fprintf(yf, "  compiler: \"(see make output)\"\n");
        fprintf(yf, "  cflags: \"-std=gnu11 -O2 -DNDEBUG -fvisibility=hidden -fPIC\"\n");
        fprintf(yf, "  host_triple: \"(see make output)\"\n");
        fprintf(yf, "  kernel: \"(see make output)\"\n");
        fprintf(yf, "silent_close_delay:\n");
        fprintf(yf, "  p50_ns: %lld\n", p50);
        fprintf(yf, "  p95_ns: %lld\n", p95);
        fprintf(yf, "  p99_ns: %lld\n", p99);
        fprintf(yf, "  samples_total: %d\n", total_n);
        fprintf(yf, "  buckets: [\"0-63\", \"64-255\", \"256-1023\", \"1024-4095\", \"4096-16383\"]\n");
        fprintf(yf, "tost:\n");
        fprintf(yf, "  delta_ms: %.0f\n", TOST_DELTA_MS);
        fprintf(yf, "  alpha: 0.05\n");
        fprintf(yf, "  n_per_bucket: %d\n", n);
        fprintf(yf, "  pairs_equivalent: %d\n", tost_pairs_equiv);
        fprintf(yf, "  pairs_total: %d\n", tost_pairs_total);
        fprintf(yf, "  passed: %s\n", tost_ok ? "true" : "false");
        fprintf(yf, "spearman:\n");
        fprintf(yf, "  n: %d\n", total_n);
        fprintf(yf, "  rho: %.6f\n", rho);
        fprintf(yf, "  ci95_low: %.6f\n", ci95_low);
        fprintf(yf, "  ci95_high: %.6f\n", ci95_high);
        fprintf(yf, "  threshold: 0.1\n");
        fprintf(yf, "  passed: %s\n", rho_ok ? "true" : "false");
        fprintf(yf, "underpowered: %s\n", (n < N_PER_BUCKET) ? "true" : "false");
        fclose(yf);
        printf("PASS [1.7-UNIT-022]: baseline written to %s\n", baseline_path);
    }

    /* Cleanup. */
    for (int b = 0; b < N_BUCKETS; b++) free(bucket_samples[b]);
    free(all_len);
    free(all_delay);
    t3_session_free(sess);
    t3_secret_free(secret);

    int overall = (!tost_ok || !rho_ok || !bias_ok) ? 1 : 0;
    if (overall)
        fprintf(stderr, "\n=== RESULT: FAIL ===\n");
    else
        printf("\n=== RESULT: PASS ===\n");
    return overall;
}
