---
spec_version: 0.1.0-draft
last_updated: 2026-04-24
status: draft
---

> **Normative.** This document defines required behaviour for conforming
> Type3 implementations. Code in `lib/` is illustrative; where this
> document and `lib/` differ, this document wins.

# Anti-Probe Behaviour

Rules that make a Type3 server indistinguishable from a generic
HTTPS+WebSocket origin under DPI probing.

## 1. Silent close

_TBD._ The server MUST close misbehaving connections without emitting
a protocol-level error. Acceptable close mechanisms.

## 2. Timing constraints

_TBD._ Response timings that MUST fall within envelope X to avoid a
fingerprintable signature.

## 3. FR43 retry heuristic

_TBD._ Client-side retry schedule after silent close. Backoff, jitter,
and the conditions under which a client concludes the endpoint is
blocked vs. transiently unavailable.

## 4. Logging

_TBD._ Server-local admin counters (e.g. `bad_header_drops`) MUST NOT
leak information useful to a probe. Logs never reference the probing
source in a way a compromised admin could be compelled to hand over.
