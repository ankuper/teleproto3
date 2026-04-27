/*
 * net-type3-dispatch.c — fork-local Type3 dispatch hook implementation.
 *
 * Bridges upstream MTProxy's CRYPTO_INIT to libteleproto3. All wire-
 * format decisions live in the library; this file is glue.
 *
 * Implements story 1-8 Tasks 1.1–1.5: dispatch logic, kill-switch poll,
 * silent-close scheduling, and ban-list-clean user-visible strings.
 * See spec/wire-format.md §1–3 and spec/anti-probe.md §1 for the
 * normative behaviour this glue must honour.
 */

#define _GNU_SOURCE 1
#include <stdint.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#include "net-type3-dispatch.h"
#include "net-type3-stats.h"
#include "t3.h"

/* Upstream headers pulled in by the subtree import (Task 0) */
#include "net-connections.h"
#include "net-msg.h"
#include "jobs/jobs.h"
#include "common/precise-time.h"
#include "common/kprintf.h"

/* ------------------------------------------------------------------ *
 * Build-time feature gate (AR-S2).                                    *
 * Compile with -DTELEPROTO3_DISPATCH_HOOK=0 to produce an upstream-  *
 * identical binary (used by additivity.yml and baseline capture).     *
 * ------------------------------------------------------------------ */
#ifndef TELEPROTO3_DISPATCH_HOOK
#  define TELEPROTO3_DISPATCH_HOOK 1
#endif

/* ------------------------------------------------------------------ *
 * Kill-switch state                                                   *
 * ------------------------------------------------------------------ */
#define KILL_SWITCH_MARKER_PATH  "/etc/teleproxy-ws-v2/disabled"

typedef enum {
    KS_MODE_DRAIN       = 0,
    KS_MODE_HARD_CLOSE  = 1
} kill_switch_mode_t;

static kill_switch_mode_t g_ks_mode  = KS_MODE_DRAIN;
static int                g_ks_active = 0; /* 1 = marker present */

/* teleproto3_kill_switch_state values:
 *   0 = idle (marker absent)
 *   1 = draining (mode=drain, marker present)
 *   2 = hard_closed (mode=hard-close, marker present) */

/* ------------------------------------------------------------------ *
 * Dispatch initialisation (Task 1.2 prototype)                        *
 * ------------------------------------------------------------------ */
int type3_dispatch_init(const char *kill_switch_mode) {
    if (!kill_switch_mode || strcmp(kill_switch_mode, "drain") == 0) {
        g_ks_mode = KS_MODE_DRAIN;
    } else if (strcmp(kill_switch_mode, "hard-close") == 0) {
        g_ks_mode = KS_MODE_HARD_CLOSE;
    } else {
        kprintf("type3_dispatch_init: unknown kill-switch mode '%s' "
                   "(expected 'drain' or 'hard-close')\n", kill_switch_mode);
        return -1;
    }
    /* Start idle */
    type3_stats.kill_switch_state = 0;
    vkprintf(1, "type3_dispatch: initialised (kill-switch mode=%s)\n",
            kill_switch_mode ? kill_switch_mode : "drain");
    return 0;
}

/* ------------------------------------------------------------------ *
 * Kill-switch poll (Task 1.4)                                         *
 * Called at 1 Hz from main loop. stat() the marker file; on          *
 * transition to present → set kill_switch_state; on absent → reset.  *
 *                                                                     *
 * Worst-case activation latency: 1000 ms (one polling tick).          *
 * A marker create followed by delete within one tick is NOT           *
 * guaranteed to be observed. Operators MUST hold the marker for ≥1 s. *
 * ------------------------------------------------------------------ */
void type3_dispatch_kill_switch_poll(void) {
    struct stat st;
    int present = (stat(KILL_SWITCH_MARKER_PATH, &st) == 0);

    if (present && !g_ks_active) {
        /* Transition: absent → present */
        g_ks_active = 1;
        int new_state = (g_ks_mode == KS_MODE_DRAIN) ? 1 : 2;
        __atomic_store_n(&type3_stats.kill_switch_state, new_state, __ATOMIC_RELAXED);
        vkprintf(1, "type3_dispatch: kill-switch activated (state=%d)\n", new_state);
    } else if (!present && g_ks_active) {
        /* Transition: present → absent */
        g_ks_active = 0;
        __atomic_store_n(&type3_stats.kill_switch_state, 0, __ATOMIC_RELAXED);
        vkprintf(1, "type3_dispatch: kill-switch deactivated\n");
    }
}

/* ------------------------------------------------------------------ *
 * Silent-close scheduling (Task 1.3)                                  *
 *                                                                     *
 * Uses job_timer_insert(C, wakeup_time) to schedule a connection      *
 * close after the sampled delay. The upstream alarm handler           *
 * (tcp_rpcs_ext_alarm) will fire at wakeup_time. We mark the         *
 * connection with C_STOPPARSE so no further data is processed.        *
 *                                                                     *
 * For v0.1.0 we do NOT store per-connection state to distinguish our  *
 * silent-close timer from other timers — we rely on the upstream      *
 * alarm handler calling fail_connection when timer fires. This is safe *
 * because we set C_STOPPARSE, so parse_execute is never re-entered.   *
 * ------------------------------------------------------------------ */
void type3_dispatch_silent_close(connection_job_t C, uint64_t delay_ns) {
    struct connection_info *c = CONN_INFO(C);

    /* Prevent further parsing on this connection */
    __sync_fetch_and_or(&c->flags, C_STOPPARSE);

    /* Schedule close after delay; upstream alarm fires fail_connection */
    double delay_sec = (double)delay_ns / 1.0e9;
    if (delay_sec < 0.001) { delay_sec = 0.001; }   /* floor: 1 ms */
    if (delay_sec > 1.0)   { delay_sec = 1.0;   }   /* ceiling: 1 s (spec §8) */

    job_timer_insert(C, precise_now + delay_sec);
    vkprintf(2, "type3_dispatch: silent-close scheduled in %.3f s\n", delay_sec);
}

/* ------------------------------------------------------------------ *
 * Main dispatch function (Task 1.1)                                   *
 *                                                                     *
 * Called from net-tcp-rpc-ext-server.c AR-S2 insertion point after   *
 * WS frame unwrap when c->ws_state == WS_STATE_ACTIVE && !c->crypto. *
 *                                                                     *
 * Reads 4-byte Session Header from c->in (peek, no consume),         *
 * parses via t3_header_parse(), does version negotiation, and         *
 * returns the dispatch outcome.                                        *
 * ------------------------------------------------------------------ */
#if TELEPROTO3_DISPATCH_HOOK

type3_dispatch_outcome_t
type3_dispatch_on_crypto_init(connection_job_t C) {
    struct connection_info *c = CONN_INFO(C);

    /* ---------------------------------------------------------- *
     * Kill-switch check: if active in hard-close mode, reject all *
     * new Type3 connections immediately. Drain mode lets          *
     * established (accepted) connections continue — but this is   *
     * a pre-accept path, so we reject in both modes here.         *
     * ---------------------------------------------------------- */
    int ks = __atomic_load_n(&type3_stats.kill_switch_state, __ATOMIC_RELAXED);
    if (ks != 0) {
        vkprintf(2, "type3_dispatch: kill-switch active (state=%d), silent-close\n", ks);
        type3_stats_silent_close_rate_limited();
        type3_stats_incr_type3_silent_close();

        /* Sample delay from a stub session (no secret context available
         * here — use fixed 50 ms floor per anti-probe §8 minimum). */
        type3_dispatch_silent_close(C, 50 * 1000000ULL /* 50 ms in ns */);
        return TYPE3_DISPATCH_DROP_SILENT;
    }

    /* ---------------------------------------------------------- *
     * Session Header parse                                         *
     * ---------------------------------------------------------- */
    if (c->in.total_bytes < 4) {
        /* Not enough data yet — pass through to let upstream buffer more */
        type3_stats_incr_type1_passthrough();
        return TYPE3_DISPATCH_PASSTHROUGH;
    }

    uint8_t hdr_buf[4];
    if (rwm_fetch_lookup(&c->in, hdr_buf, 4) != 4) {
        /* Internal error; passthrough to let upstream handle */
        type3_stats_incr_type1_passthrough();
        return TYPE3_DISPATCH_PASSTHROUGH;
    }

    /* ---------------------------------------------------------- *
     * Detect Type3 command byte (0x03 per wire-format.md §3).    *
     * Non-0x03 command_type bytes are not Type3 — passthrough.   *
     * ---------------------------------------------------------- */
    if (hdr_buf[0] != 0x03) {
        /* Type1 or Type2: passthrough */
        if (hdr_buf[0] == 0x01) {
            type3_stats_incr_type1_passthrough();
        } else {
            type3_stats_incr_type2_passthrough();
        }
        return TYPE3_DISPATCH_PASSTHROUGH;
    }

    /* ---------------------------------------------------------- *
     * Parse the 4-byte Type3 Session Header                       *
     * ---------------------------------------------------------- */
    t3_header_t hdr;
    t3_result_t rc = t3_header_parse(hdr_buf, &hdr);
    if (rc != T3_OK) {
        vkprintf(2, "type3_dispatch: t3_header_parse failed (%s)\n",
                 t3_strerror(rc));
        type3_stats_incr_type3_bad_header();
        type3_stats_silent_close_unknown_cmd();
        type3_stats_incr_type3_silent_close();

        /* Sample a fixed 50–200 ms delay (no session available; use midpoint). */
        type3_dispatch_silent_close(C, 100 * 1000000ULL);
        return TYPE3_DISPATCH_DROP_SILENT;
    }

    /* ---------------------------------------------------------- *
     * Version check (wire-format.md §6).                          *
     * We do not have a t3_session_t here (no secret loaded from   *
     * config yet in v0.1.0 dispatch path — the session is per-DC  *
     * connection, not per-incoming-connection). For v0.1.0 we     *
     * enforce version == 1 directly.                               *
     *                                                              *
     * TODO(server-v0.2.0): load the operator secret at startup,   *
     * create a per-server t3_secret_t + t3_session_t, and use     *
     * t3_session_negotiate_version(sess, hdr.version, &action).   *
     * ---------------------------------------------------------- */
    if (hdr.version != 1) {
        vkprintf(2, "type3_dispatch: unsupported version %u\n", hdr.version);
        type3_stats_incr_type3_bad_header();
        type3_stats_silent_close_bad_version();
        type3_stats_incr_type3_silent_close();

        type3_dispatch_silent_close(C, 100 * 1000000ULL);
        return TYPE3_DISPATCH_DROP_SILENT;
    }

    /* ---------------------------------------------------------- *
     * Flags validation (wire-format.md §3; reserved bits must be  *
     * zero in v0.1.0 per the ABI freeze story 1-6).               *
     * ---------------------------------------------------------- */
    if (hdr.flags != 0) {
        vkprintf(2, "type3_dispatch: non-zero reserved flags 0x%04x\n",
                 hdr.flags);
        type3_stats_incr_type3_bad_header();
        type3_stats_silent_close_bad_flags();
        type3_stats_incr_type3_silent_close();

        type3_dispatch_silent_close(C, 100 * 1000000ULL);
        return TYPE3_DISPATCH_DROP_SILENT;
    }

    /* ---------------------------------------------------------- *
     * Accept: valid Type3 Session Header                           *
     * ---------------------------------------------------------- */
    type3_stats_incr_type3_accept();
    vkprintf(2, "type3_dispatch: accepted Type3 connection (version=%u)\n",
             hdr.version);
    return TYPE3_DISPATCH_ACCEPT;
}

#else  /* TELEPROTO3_DISPATCH_HOOK == 0 — upstream-identical stub */

type3_dispatch_outcome_t
type3_dispatch_on_crypto_init(connection_job_t C) {
    (void)C;
    return TYPE3_DISPATCH_PASSTHROUGH;
}

#endif /* TELEPROTO3_DISPATCH_HOOK */
