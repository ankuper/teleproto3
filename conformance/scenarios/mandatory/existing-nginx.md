---
status: draft
last_updated: 2026-04-27
---

> **Normative.** This document defines required behaviour for conforming
> Type3 implementations. Code in `lib/` is illustrative; where this
> document and `lib/` differ, this document wins.

# Scenario: Existing nginx Coexistence

**Compliance level:** mandatory  
**Owner:** Epic 4 (existing-nginx coexistence)  
**Status:** draft — scenario body pending; path reserved by Story 1.9.

## Purpose

This scenario validates correct operation of a Type3 server
deployed alongside an already-running nginx instance on the same VPS
(nginx on port 443; Type3 server on a non-standard port, fronted by
an nginx `stream {}` <!-- ban-list-doc: technical identifier -->proxy_pass<!-- /ban-list-doc --> block).

The IUT must:

1. Operate correctly when all Type3 traffic is forwarded by nginx via
   TCP connection (no TLS termination).
2. Parse the operator-supplied Type3 secret and handle the obfuscated
   handshake identically to the greenfield case.
3. Silently close on probe-like connections even when the connection
   arrives through the nginx forwarding layer.

## Vectors consumed

- `conformance/vectors/unit.json#secret-format` (accept + reject cases).
- `conformance/vectors/unit.json#session-header` (mandatory cases).

## IUT pre-conditions

- nginx `stream {}` block configured with `<!-- ban-list-doc: technical identifier -->proxy_pass<!-- /ban-list-doc --> localhost:<port>`.
- Type3 server binary running on the proxied port.
- Operator secret set to the test secret from Story 1.1 vector set.

## Expected outcomes

_TBD — populated when Epic 4 (nginx coexistence) lands._
