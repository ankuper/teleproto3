# Runbook: migration from v1 to v2 Type3

Migrating existing v1 (legacy teleproxy) clients onto v2 (teleproto3)
with zero-downtime.

## Context

The production host (94.156.131.252 via nip.io + CF Worker) runs v1
live. v2 is introduced alongside v1 on the same host, runs in
coexistence mode, and users migrate as they update their clients.

## Prerequisites

- `coexistence_v1_v2_test.sh` has passed locally and in CI against
  the target `server-vX.Y.Z` tag.
- The v2 secret has been generated and distributed to a small alpha
  cohort.
- The CF Worker + installer pipeline is ready to hand out v2 secrets
  to new installs.

## Procedure

_TBD(server-v0.1.0):_ finalise. Intended shape:

1. **Deploy v2-capable server** with both v1 and v2 secrets configured.
   v1 clients continue to work unchanged.
2. **Alpha cohort** — switch a small pool of installer tokens to hand
   out v2 secrets. Monitor `connections_accepted` split by version for
   48h.
3. **Widen rollout** — advance the installer to hand v2 to all new
   installs. Existing v1 users keep working.
4. **Sunset announcement** — once v2 CF-worker/installer has been
   stable for ≥30 days and v1 traffic has dropped below 5%, announce
   a v1 sunset window in-client via the `degraded` UX state.
5. **v1 shutdown** — after the announced window, remove the v1 secret
   from the server config. Retain `coexistence_v1_v2_test.sh` in CI
   for regression coverage while any v1 deployment remains anywhere.

## Rollback

Any step is reversible up to step 5. Keep the v1 secret in the server
config until v1 is fully sunset.

## Observables

- `connections_accepted` (v1 vs v2 bucket) — main migration metric.
- `bad_header_drops` — increase during alpha MAY indicate a
  mis-distributed secret, not an attack. Investigate before pulling
  the kill switch.
