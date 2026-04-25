---
spec_version: 0.1.0-draft
last_updated: 2026-04-24
status: draft
---

> **Normative.** This document defines required behaviour for conforming
> Type3 implementations. Code in `lib/` is illustrative; where this
> document and `lib/` differ, this document wins.

# Wire Format

The end-to-end shape of a Type3 connection: TLS → WebSocket upgrade →
Session Header → obfuscated-2 MTProto. This is the document
implementers live in.

## 1. Connection establishment

_TBD._ TLS handshake, HTTP(S) upgrade headers, Sec-WebSocket-Key /
Sec-WebSocket-Accept rules. See diagram
[`diagrams/connection-flow.mmd`](diagrams/connection-flow.mmd).

## 2. WebSocket framing

_TBD._ Client frames MUST be masked per RFC 6455. Server frames MUST NOT
be masked. Fragmentation permitted; reassembly rules.

## 3. Session Header

_TBD._ Fixed-size preamble, layout, and parsing rules. See diagram
[`diagrams/session-header.mmd`](diagrams/session-header.mmd).

## 4. Obfuscated-2 payload

_TBD._ AES-256-CTR setup from the pre-shared key, IV derivation, and
the framing rules that carry MTProto packets inside WebSocket frames.

## 5. Errors and silent close

_TBD._ Normative error handling. Cross-references with
[`anti-probe.md`](anti-probe.md).

## 6. Version Negotiation

_TBD._ How a client advertises supported versions and how a server
selects. Required for forward compatibility without re-running the
secret-distribution flow.
