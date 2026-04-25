---
spec_version: 0.1.0-draft
last_updated: 2026-04-24
status: draft
---

> **Normative.** This document defines required behaviour for conforming
> Type3 implementations. Code in `lib/` is illustrative; where this
> document and `lib/` differ, this document wins.

# Non-Goals

Type3 is a transport for MTProto. It is not a general-purpose
anti-censorship or anti-surveillance protocol, and it explicitly does
not defend against the following. Implementers and auditors MUST read
this section before concluding Type3 is broken.

## Explicitly out of scope

_TBD._ Enumerate. Seed list (subject to refinement):

1. **Global passive adversary** — an attacker with total network-wide
   view can correlate traffic volumes and timings; Type3 makes no
   attempt to defeat this.
2. **Traffic-volume fingerprinting** — packet-size and burst-pattern
   fingerprinting over long horizons. Mitigations (padding, cover
   traffic) are client-layer choices, not Type3 MUSTs.
3. **Endpoint compromise** — Type3 does not defend the client OS, the
   server host, nor MTProto's own session keys against endpoint-level
   attackers.
4. **CDN coercion at scale** — a nation-state compelling Cloudflare to
   MITM a specific SNI is outside scope; we assume CDN operators push
   back or ship clients to alternate providers.
5. **Metadata at the MTProto layer** — who talks to whom, message
   cadence, group membership. These are MTProto concerns.
6. **Quantum adversary** — this spec does not specify post-quantum
   primitives. A future MAJOR version MAY.

## Why a non-goals list

Security is only as meaningful as its scope. Publishing what we *don't*
try to do is how implementers can layer correctly, and how reviewers
can critique without misattributing weaknesses.
