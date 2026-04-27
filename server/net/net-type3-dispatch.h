/*
 * net-type3-dispatch.h — fork-local Type3 dispatch hook.
 *
 * Exposed to the upstream MTProxy connection-init path. Wired into
 * CRYPTO_INIT in net-tcp-rpc-ext-server.c; from there it calls into
 * libteleproto3 via ../lib/include/t3.h and returns a decision:
 * accept, reject (silent-close), or pass-through to existing non-Type3
 * transports.
 *
 * This file is fork-local (Type3 additions). Upstream-managed net/
 * files are not modified.
 */

#ifndef TELEPROTO3_SERVER_NET_TYPE3_DISPATCH_H
#define TELEPROTO3_SERVER_NET_TYPE3_DISPATCH_H

#include "net-connections.h"

/* Dispatch outcomes.
 * Values are ABI-committed for v0.1.0 (admin scraper compatibility). */
typedef enum {
    TYPE3_DISPATCH_ACCEPT       = 0,
    TYPE3_DISPATCH_PASSTHROUGH  = 1,  /* not a Type3 connection */
    TYPE3_DISPATCH_DROP_SILENT  = 2   /* malformed; close without response */
} type3_dispatch_outcome_t;

/* Called from CRYPTO_INIT once the WS upgrade and Session Header are
 * readable on `C`. Must not block. */
type3_dispatch_outcome_t type3_dispatch_on_crypto_init(connection_job_t C);

/* Initialise dispatcher state.
 * kill_switch_mode: "drain" (default) or "hard-close".
 * Returns 0 on success, -1 on bad mode string. */
int  type3_dispatch_init(const char *kill_switch_mode);

/* Poll kill-switch marker file (call at 1 Hz from main loop).
 * Reads /etc/teleproxy-ws-v2/disabled; updates kill_switch_state gauge. */
void type3_dispatch_kill_switch_poll(void);

/* Initiate a delayed silent close on connection C.
 * Samples delay via t3_silent_close_delay_sample_ns (lib API),
 * schedules FIN via job_timer_insert(C, now + delay_sec),
 * and increments the appropriate silent-close counter. */
void type3_dispatch_silent_close(connection_job_t C, uint64_t delay_ns);

#endif /* TELEPROTO3_SERVER_NET_TYPE3_DISPATCH_H */
