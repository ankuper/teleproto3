/*
 * net-type3-dispatch.c — fork-local Type3 dispatch hook implementation.
 *
 * Bridges upstream MTProxy's CRYPTO_INIT to libteleproto3. All wire-
 * format decisions live in the library; this file is glue.
 *
 * TODO(server-v0.1.0): implement once the first subtree pull has landed
 * and the connection struct (net-connections.h) is available. See
 * spec/wire-format.md §1–3 and spec/anti-probe.md §1 for the normative
 * behaviour this glue must honour.
 */

#include "net-type3-dispatch.h"
#include "net-type3-stats.h"
#include "t3.h"

type3_dispatch_outcome_t
type3_dispatch_on_crypto_init(struct connection *c)
{
    (void)c;
    /* Placeholder: pass through until lib-v0.1.0 + server-v0.1.0 land
     * the real dispatch. A passthrough keeps existing non-Type3
     * transports working during scaffold. */
    return TYPE3_DISPATCH_PASSTHROUGH;
}
