---
spec_version: 0.1.0-draft
last_updated: 2026-04-24
status: draft
---

> **Normative.** This document defines required behaviour for conforming
> Type3 implementations. Code in `lib/` is illustrative; where this
> document and `lib/` differ, this document wins.

# Type3 Specification

Type3 (also: `mtProxy3`) is a transport for tunneling MTProto over
WebSocket-upgraded HTTPS. This directory contains the normative
specification.

## Reading order

1. [`glossary.md`](glossary.md) — terms of art used throughout the spec.
2. [`threat-model.md`](threat-model.md) — the adversary, their
   capabilities, the properties Type3 defends.
3. [`non-goals.md`](non-goals.md) — what Type3 explicitly does NOT defend
   against. Read this before concluding Type3 is "broken".
4. [`compliance-levels.md`](compliance-levels.md) — Core / Full /
   Extended tiers.
5. [`secret-format.md`](secret-format.md) — the `0xff` + 16-byte key +
   domain encoding.
6. [`wire-format.md`](wire-format.md) — handshake, framing, §6 version
   negotiation.
7. [`anti-probe.md`](anti-probe.md) — silent-close rules, timing
   constraints, FR43 retry heuristic.
8. [`ux-conformance.md`](ux-conformance.md) — three states × three
   actions for client UI.
9. [`conformance-procedure.md`](conformance-procedure.md) — how the
   language-agnostic harness in `conformance/` interrogates an
   implementation.

## Normative artefacts

- [`ux-strings.yaml`](ux-strings.yaml) — canonical en/ru/fa/zh strings.
- [`diagrams/`](diagrams/) — Mermaid/ASCII diagrams. NOT optional.
- [`assets/icon-degraded.svg`](assets/icon-degraded.svg) — single
  normative icon for the `degraded` UI state.

## Change log

- Breaking changes bump MAJOR — see [`CHANGELOG.md`](CHANGELOG.md).
- Known ambiguities live in [`ERRATA.md`](ERRATA.md) until resolved by
  the next MINOR.

## Licence

Apache 2.0 — see [`LICENSE`](LICENSE). Deliberately permissive so that
third-party implementations (including GPL-downstream) can consume the
spec without relicensing.
