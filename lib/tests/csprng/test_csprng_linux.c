/*
 * test_csprng_linux.c — Statistical smoke test for the Linux CSPRNG backend.
 *
 * Draws 1 MiB via t3_csprng_bytes, computes a chi-square goodness-of-fit
 * statistic against a uniform distribution over all 256 byte values.
 *
 * Expected chi-square: mean = 255, stddev ≈ 22.58 (df = 255).
 * Acceptance window: [142, 368] = mean ± 5σ.
 *
 * A failure here means the CSPRNG produces obviously non-uniform output.
 * It is not a rigorous RNG test; it catches gross failures (constant output,
 * stuck bits, all-zeros).
 *
 * Story 1-12 (AC #2).
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>

#include "t3.h"
#include "t3_csprng.h"

#define SAMPLE_BYTES  (1024u * 1024u)   /* 1 MiB */
#define NUM_BUCKETS   256u
#define EXPECTED      ((double)SAMPLE_BYTES / (double)NUM_BUCKETS)   /* 4096.0 */

/* chi-square ±5σ window for df = 255 */
#define CHI2_LO  142.0
#define CHI2_HI  368.0

int main(void) {
    uint8_t *buf = malloc(SAMPLE_BYTES);
    if (!buf) {
        fprintf(stderr, "test_csprng_linux: malloc failed\n");
        return 1;
    }

    t3_result_t rc = t3_csprng_bytes(buf, SAMPLE_BYTES);
    if (rc != T3_OK) {
        fprintf(stderr, "test_csprng_linux: t3_csprng_bytes returned %s\n",
                t3_strerror(rc));
        free(buf);
        return 1;
    }

    unsigned long freq[NUM_BUCKETS];
    memset(freq, 0, sizeof(freq));
    for (size_t i = 0; i < SAMPLE_BYTES; i++) {
        freq[buf[i]]++;
    }
    free(buf);

    double chi2 = 0.0;
    for (unsigned i = 0; i < NUM_BUCKETS; i++) {
        double diff = (double)freq[i] - EXPECTED;
        chi2 += (diff * diff) / EXPECTED;
    }

    printf("chi2 = %.4f  window [%.1f, %.1f]\n", chi2, CHI2_LO, CHI2_HI);

    if (chi2 < CHI2_LO || chi2 > CHI2_HI) {
        fprintf(stderr,
                "FAIL: chi-square %.4f outside ±5σ window [%.1f, %.1f]\n",
                chi2, CHI2_LO, CHI2_HI);
        return 1;
    }

    printf("PASS\n");
    return 0;
}
