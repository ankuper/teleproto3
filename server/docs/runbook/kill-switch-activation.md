# Runbook: kill-switch activation

When an active probe campaign is detected and a server endpoint must
stop answering Type3 flows immediately.

## When to pull the switch

- Sustained spike in `bad_header_drops` (>10x baseline for >15min).
- A confirmed external compromise signal (maintainer escalation).
- A pre-announced retirement of an endpoint.

Do NOT pull the switch for routine traffic variation or single-source
scans — silent-close already handles those.

## Procedure

_TBD(server-v0.1.0):_ finalise. Intended shape:

1. Flip the `TYPE3_ACCEPT_ENABLED` flag in the server config to `0`.
2. `systemctl reload mtproto-proxy` — the dispatch hook now returns
   `TYPE3_DISPATCH_DROP_SILENT` for all incoming Type3 handshakes.
3. Confirm via counters: `connections_accepted` flatlines,
   `silent_closes` climbs.
4. **Do NOT 503.** Do NOT return any protocol-level error. The
   endpoint must look either indistinguishable-from-idle or behave
   exactly as a generic HTTPS origin.

## Recovery

- Investigate via the observability runbook (TBD) before re-enabling.
- Rotate the Type3 secret if compromise is suspected — rotation is a
  client-visible event, so coordinate with the installer bump (see
  [`migration-from-v1.md`](migration-from-v1.md) for the user-visible
  flow).
- Re-enable by flipping the flag back; log the incident in a private
  ops log (server-local, never remote).

## Post-incident

Open a private post-mortem. Redact remote-source identifiers per
spec/anti-probe.md §4.
