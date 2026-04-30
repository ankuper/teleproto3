/*
 * net-type3-dispatch.h — fork-local Type3 dispatch hook interface.
 *
 * Bridges upstream MTProxy's CRYPTO_INIT to libteleproto3. All wire-
 * format decisions live in the library; this file is glue.
 *
 * See spec/wire-format.md §1–3 and spec/anti-probe.md §1 for the
 * normative behaviour this glue must honour.
 *
 * Stability: this header is fork-local (teleproto3/server only).
 * It is NOT part of the libteleproto3 public ABI (see lib/include/t3.h).
 */

#ifndef NET_TYPE3_DISPATCH_H
#define NET_TYPE3_DISPATCH_H

#include <stdint.h>
#include "net-connections.h"

/* --------------------------------------------------------------------
 * Dispatch outcomes (v0.1.0 ABI commitment — do NOT renumber)
 * Consumed by net-tcp-rpc-ext-server.c AR-S2 hook and by test harnesses.
 * -------------------------------------------------------------------- */
typedef enum {
    TYPE3_DISPATCH_PASSTHROUGH  = 0, /* not a Type3 session — fall through */
    TYPE3_DISPATCH_ACCEPT       = 1, /* valid Type3 header — proceed to obf2 */
    TYPE3_DISPATCH_DROP_SILENT  = 2  /* bad/probe header — silent close */
} type3_dispatch_outcome_t;

/* --------------------------------------------------------------------
 * Lifecycle / configuration
 * -------------------------------------------------------------------- */

/*
 * type3_dispatch_init — initialise dispatcher state.
 *
 * kill_switch_mode: "drain" (default) or "hard-close".
 *   drain:      new connections are silently closed; existing finish normally.
 *   hard-close: all connections are silently closed on next poll tick.
 * Returns 0 on success, -1 on unrecognised mode string.
 */
int  type3_dispatch_init(const char *kill_switch_mode);

/*
 * type3_dispatch_cleanup — free dispatcher state on shutdown.
 */
void type3_dispatch_cleanup(void);

/* --------------------------------------------------------------------
 * Per-connection entry point (called from AR-S2 hook in ext-server)
 * -------------------------------------------------------------------- */

/*
 * type3_dispatch_on_crypto_init — inspect first 4 bytes of a WS session.
 *
 * Called when ws_state == WS_STATE_ACTIVE && !c->crypto.
 * Reads the 4-byte Session Header from c->in, calls libteleproto3 parser,
 * and returns the appropriate outcome.
 */
type3_dispatch_outcome_t type3_dispatch_on_crypto_init(connection_job_t C);

/* --------------------------------------------------------------------
 * Kill-switch (FR18, AR-S3)
 * -------------------------------------------------------------------- */

/*
 * type3_dispatch_kill_switch_poll — check marker file at 1 Hz.
 *
 * Call once per second from the main loop.
 * Marker path: /etc/teleproxy-ws-v2/disabled
 * State transitions:
 *   absent               → idle       (0)
 *   present + drain      → draining   (1)
 *   present + hard-close → hard_closed (2)
 */
void type3_dispatch_kill_switch_poll(void);

/* --------------------------------------------------------------------
 * Silent-close helper (Task 1.3)
 * -------------------------------------------------------------------- */

/*
 * type3_dispatch_silent_close — schedule silent FIN after delay_ns nanoseconds.
 *
 * Does NOT send a WebSocket close frame (AC #2).
 * Uses upstream job_timer_insert to schedule via the connection alarm handler.
 */
void type3_dispatch_silent_close(connection_job_t C, uint64_t delay_ns);

#endif /* NET_TYPE3_DISPATCH_H */
