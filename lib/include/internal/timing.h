/*
 * timing.h — internal rejection-sampling uniform-delay helper.
 *
 * This is the reference implementation of the Type3 protocol.
 * Normative behaviour is defined in spec/. Where they differ, spec/ wins.
 *
 * Stability: internal header; NOT exported; NOT ABI-stable.
 */

#ifndef T3_INTERNAL_TIMING_H
#define T3_INTERNAL_TIMING_H

#include <stdint.h>
#include "t3.h"

__attribute__((visibility("hidden")))
int t3_timing_rejection_sample_uniform_ns(const t3_callbacks_t *cb,
                                          uint64_t lo_ns,
                                          uint64_t hi_ns,
                                          uint64_t *out_ns);

#endif /* T3_INTERNAL_TIMING_H */
