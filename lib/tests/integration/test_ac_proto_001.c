/*
 * test_ac_proto_001.c — AC-PROTO-001 integration test.
 *
 * Context (Amelia's ruling): this is a property of the LIBRARY's timer
 * behaviour, not of the server or of a client. The test runs a
 * Kolmogorov-Smirnov test on the timer's jitter distribution to prove
 * it matches the spec's required envelope.
 *
 * Runs in lib-level CI (build-lib.yml). Lives under tests/integration/
 * rather than unit/ because it needs realistic timing.
 *
 * TODO(lib-v0.1.0): implement KS test harness + sampling loop.
 */

#include "t3.h"

int main(void) {
    return 0;
}
