/*
 * net-type3-stats.h — admin-local counters for Type3 dispatch.
 *
 * Rule (Cat 11): server-local observability ONLY. No remote emission.
 * Counters are readable via the existing MTProxy admin interface.
 *
 * Normative note: counter names appearing in logs/admin UI MUST NOT
 * leak information useful to a probe (see spec/anti-probe.md §4).
 */

#ifndef TELEPROTO3_SERVER_NET_TYPE3_STATS_H
#define TELEPROTO3_SERVER_NET_TYPE3_STATS_H

#include <stdint.h>

typedef struct {
    uint64_t connections_accepted;
    uint64_t connections_passthrough;
    uint64_t bad_header_drops;      /* Cat 11 */
    uint64_t silent_closes;
} type3_stats_t;

extern type3_stats_t type3_stats;

void type3_stats_incr_accept(void);
void type3_stats_incr_passthrough(void);
void type3_stats_incr_bad_header(void);
void type3_stats_incr_silent_close(void);

#endif /* TELEPROTO3_SERVER_NET_TYPE3_STATS_H */
