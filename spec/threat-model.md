---
spec_version: 0.1.0-draft
last_updated: 2026-04-24
status: draft
---

> **Normative.** This document defines required behaviour for conforming
> Type3 implementations. Code in `lib/` is illustrative; where this
> document and `lib/` differ, this document wins.

# Threat Model

Before reading what Type3 defends, read [`non-goals.md`](non-goals.md)
— knowing what is *out of scope* matters as much as what is in scope.

## The adversary

_TBD._ Characterise the DPI/TSPU-class adversary that motivates this
protocol. Classic model elements to include:

- **Vantage:** on-path between client and server, or adjacent to either.
- **Capabilities:** passive observation, active injection, TLS
  termination at gateways where laws/infrastructure allow, rate-limited
  probing of candidate servers.
- **Constraints:** cannot break TLS of unrelated CDNs wholesale; cannot
  compel Cloudflare/Fastly to hand over traffic at protocol scale;
  operates under per-flow CPU budgets that preclude deep-path heuristics.

## What the adversary knows

_TBD._ Public IPs, nip.io domain shapes, published client versions.

## What the adversary can do

_TBD._ Enumerate active/passive attack primitives this spec defends
against.

## Defended properties

_TBD._ Confidentiality of MTProto payload, indistinguishability of
Type3 flows from benign TLS+WebSocket traffic under sampled DPI,
liveness under blocklist growth.

---

_Scaffold note:_ drafted alongside [`wire-format.md`](wire-format.md)
and [`anti-probe.md`](anti-probe.md) in the spec-v0.1.0 epic.
