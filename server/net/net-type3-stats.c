/*
 * net-type3-stats.c — simple atomic counters for Type3 dispatch.
 *
 * TODO(server-v0.1.0): upgrade to per-CPU counters once wired into
 * upstream's job/thread model. The single-global variant here is
 * acceptable for v0.1.0 admin read-outs.
 */

#include "net-type3-stats.h"

type3_stats_t type3_stats = {0};

void type3_stats_incr_accept(void)       { __atomic_add_fetch(&type3_stats.connections_accepted,   1, __ATOMIC_RELAXED); }
void type3_stats_incr_passthrough(void)  { __atomic_add_fetch(&type3_stats.connections_passthrough, 1, __ATOMIC_RELAXED); }
void type3_stats_incr_bad_header(void)   { __atomic_add_fetch(&type3_stats.bad_header_drops,        1, __ATOMIC_RELAXED); }
void type3_stats_incr_silent_close(void) { __atomic_add_fetch(&type3_stats.silent_closes,           1, __ATOMIC_RELAXED); }
