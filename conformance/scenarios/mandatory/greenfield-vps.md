---
status: draft
last_updated: 2026-04-27
---

> **Normative.** This document defines required behaviour for conforming
> Type3 implementations. Code in `lib/` is illustrative; where this
> document and `lib/` differ, this document wins.

# Scenario: Greenfield VPS Deployment

**Compliance level:** mandatory  
**Owner:** Epic 3 (greenfield VPS installer)  
**Status:** draft — scenario body pending; path reserved by Story 1.9.

## Purpose

This scenario validates end-to-end correct operation of a Type3 server
deployed on a greenfield VPS (no pre-existing nginx or other
service on port 443). The IUT must:

1. Parse the operator-supplied Type3 secret correctly.
2. Accept a compliant client handshake and forward MTProto traffic.
3. Silently close on any malformed or probe-like connection attempt
   (per `anti-probe.md` mandatory timing invariant).

## Vectors consumed

- `conformance/vectors/unit.json#secret-format` (accept + reject cases).
- `conformance/vectors/unit.json#session-header` (mandatory cases).

## IUT pre-conditions

- Server binary running on `localhost` at the port specified by `--endpoint`.
- Operator secret set to the test secret from Story 1.1 vector set.
- No upstream Telegram DC connection required (loopback smoke-test only).

## Expected outcomes

_TBD — populated when Epic 3 (greenfield installer) lands._
