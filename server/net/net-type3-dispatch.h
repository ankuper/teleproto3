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

/* Dispatch outcomes. */
typedef enum {
    TYPE3_DISPATCH_ACCEPT       = 0,
    TYPE3_DISPATCH_PASSTHROUGH  = 1,  /* not a Type3 connection */
    TYPE3_DISPATCH_DROP_SILENT  = 2   /* malformed; close without response */
} type3_dispatch_outcome_t;

/* Called from CRYPTO_INIT once the WS upgrade and Session Header are
 * readable on `c`. Must not block. */
type3_dispatch_outcome_t type3_dispatch_on_crypto_init(struct connection *c);

#endif /* TELEPROTO3_SERVER_NET_TYPE3_DISPATCH_H */
