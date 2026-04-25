---
spec_version: 0.1.0-draft
last_updated: 2026-04-24
status: draft
---

> **Normative.** This document defines required behaviour for conforming
> Type3 implementations. Code in `lib/` is illustrative; where this
> document and `lib/` differ, this document wins.

# UX Conformance

Client UI requirements for Type3. Normative states, transitions, and
strings. A client claiming Full compliance MUST pass every clause in
this section.

## 1. The three states

_TBD._ Canonical states: `connected`, `degraded`, `offline`. Each with a
normative definition of the condition that triggers it.

## 2. The three actions

_TBD._ Canonical user actions: `retry`, `rotate`, `report`. Each with
pre/postconditions.

## 3. State × action matrix

_TBD._ 3×3 matrix of legal and required transitions. See diagram
[`diagrams/ux-state-machine.mmd`](diagrams/ux-state-machine.mmd).

## 4. Strings

Implementations MUST source user-facing strings from
[`ux-strings.yaml`](ux-strings.yaml). Translation keys are stable;
translations MAY be refined in MINOR releases, keys cannot be renamed
without a MAJOR bump.

## 5. Icons

The `degraded` state uses the normative icon at
[`assets/icon-degraded.svg`](assets/icon-degraded.svg). Other states
use implementation-defined icons; no normative asset is prescribed.

## 6. Accessibility

_TBD._ WCAG 2.1 AA — contrast ratios, focus order, screen-reader
labels derived from the canonical strings.

## 7. Telemetry

Zero client-side telemetry. No beacons, no analytics SDKs, no phoning
home on state transitions. This is non-negotiable; see Cat 9 of the
architecture.
