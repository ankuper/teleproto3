---
spec_version: 0.1.0-draft
last_updated: 2026-04-24
status: draft
---

> **Normative.** This document defines required behaviour for conforming
> Type3 implementations. Code in `lib/` is illustrative; where this
> document and `lib/` differ, this document wins.

# Glossary

Terms of art used throughout the Type3 specification. A term listed here
carries its defined meaning everywhere in `spec/` unless another section
explicitly scopes a narrower definition.

## A–Z

**Command slot**
: _TBD._ The opaque region of the Session Header reserved for
  server-issued commands (e.g. reconfiguration, rotation hints).

**Degraded (UI state)**
: _TBD._ One of three client-facing UI states; see
  [`ux-conformance.md`](ux-conformance.md).

**Probe**
: _TBD._ An adversary connection whose purpose is to fingerprint the
  server rather than carry legitimate MTProto traffic. See
  [`anti-probe.md`](anti-probe.md).

**Session Header**
: _TBD._ The fixed-size framing preamble carried in-band with the first
  client-to-server frame after the WebSocket upgrade.

**Type3 (a.k.a. mtProxy3)**
: _TBD._ The transport profile defined by this specification —
  MTProto-over-WebSocket with pre-shared obfuscation key.

---

_Scaffold note:_ each TBD entry is fleshed out during the spec-v0.1.0
drafting epic. Definitions here are normative once published.
