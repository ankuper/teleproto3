---
spec_version: 0.1.0-draft
last_updated: 2026-04-24
status: draft
---

> **Normative.** This document defines required behaviour for conforming
> Type3 implementations. Code in `lib/` is illustrative; where this
> document and `lib/` differ, this document wins.

# Compliance Levels

A Type3 implementation MUST declare which compliance level it claims.
The conformance harness in [`conformance/`](../conformance/) executes
a disjoint scenario set per level.

## Core

_TBD._ Smallest viable subset: secret parsing, Session Header, one
handshake flow, mandatory anti-probe behaviour. Enough to interoperate
with a stock server for a single user.

Mandatory scenario set: [`conformance/scenarios/mandatory/`](../conformance/scenarios/mandatory/).

## Full

_TBD._ Core plus rotation, §6 version negotiation, UX state machine
implemented, full `ux-strings.yaml` coverage (en/ru/fa/zh).

Scenario set: [`conformance/scenarios/full/`](../conformance/scenarios/full/).

## Extended

_TBD._ Future / optional features: experimental probe heuristics,
alternate framing modes, advanced diagnostics. Claiming Extended
without Full is not permitted.

Scenario set: [`conformance/scenarios/extended/`](../conformance/scenarios/extended/).

## Reporting

An implementation advertising a compliance level MUST cite:
- The `spec-vX.Y.Z` tag it targets.
- The `conformance-vX.Y.Z` tag it was verified against.
- The harness output (PASS/FAIL per scenario).

See [`.github/ISSUE_TEMPLATE/conformance-report.md`](../.github/ISSUE_TEMPLATE/conformance-report.md).
