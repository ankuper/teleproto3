---
spec_version: 0.1.0-draft
last_updated: 2026-04-24
status: draft
---

> **Normative.** This document defines required behaviour for conforming
> Type3 implementations. Code in `lib/` is illustrative; where this
> document and `lib/` differ, this document wins.

# Conformance Procedure

Language-agnostic protocol for interrogating an implementation-under-test
(IUT). The harness implementation lives at
[`../conformance/runner/`](../conformance/runner/); this document is the
normative contract between the harness and the IUT, so harnesses in
other languages remain possible.

## 1. Harness protocol

_TBD._ stdin/stdout JSON-framed protocol. The IUT is invoked as a
subprocess; the harness sends `{"op": ..., "args": ...}` lines on
stdin and reads `{"ok": true, "result": ...}` or `{"ok": false,
"error": ...}` lines on stdout. Fully specified here so a Rust, Go,
or Python implementation can be driven from the same vectors.

## 2. Operations

_TBD._ Enumerate: `parse_secret`, `build_session_header`,
`decode_frame`, `simulate_handshake`, etc.

## 3. Vectors

The harness consumes vectors from
[`../conformance/vectors/`](../conformance/vectors/) and scenario
manifests from [`../conformance/scenarios/`](../conformance/scenarios/).
Schema version is recorded in each vector file's header.

## 4. Compliance-level mapping

- Core: `scenarios/mandatory/`
- Full: `scenarios/mandatory/` + `scenarios/full/`
- Extended: all three.

## 5. Reporting

See [`../.github/ISSUE_TEMPLATE/conformance-report.md`](../.github/ISSUE_TEMPLATE/conformance-report.md).
