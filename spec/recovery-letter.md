---
spec_version: 0.1.0-draft
last_updated: 2026-05-14
status: draft
---

> **Normative.** This document defines required behaviour for conforming
> Type3 implementations. Code in `lib/` is illustrative; where this
> document and `lib/` differ, this document wins.

# Recovery Letter

Operator disaster-recovery artefact — physical, network-blocking-resistant,
tactile, hand-annotatable. Generated once by the installer on
operator-first-run; regenerated with `--rebuild-recovery-letter`. Not a
runtime component. (Source: ux-design-specification.md §C3.)

## 1. Anatomy

Single A4 / US-Letter document, tri-foldable. The section order below is
normative and MUST NOT be rearranged.

### 1.1 Header block

- **Title.** "Recovery Letter" — unbranded, language-selectable.
- **Date of issue.** ISO 8601 (e.g. `2026-05-14`).
- **Server ID + fingerprint.** Monospace block (IBM Plex Mono).
- **Admin-contact field.** MUST be an **opaque operator handle** of the
  form `TP3-XXXXXXXX` (per ux-design-specification.md §C3 admin-contact
  convention).
  - Raw IP addresses MUST NOT appear in this field.
  - Bare hostnames MUST NOT appear in this field.
  - Rationale: the recovery letter is a physical artefact that may be
    left unattended; IP or hostname leakage constitutes a fingerprintable
    infrastructure identifier in violation of NFR19 ("Server logs do not
    contain client IP addresses at default verbosity") and NFR20 ("Error
    messages visible to the end-user are generic strings — never
    protocol-implementation detail, remote hostname, or other
    fingerprintable identifiers").

> **Disambiguation.** The §1.2 Domain block contains the operator-public
> configuration ID — the domain string is already embedded in the secret
> URL itself and is NOT subject to the Header-block privacy ban above. The
> §1.1 ban applies exclusively to the admin-contact field.

### 1.2 Domain + configuration hash

Monospace block (IBM Plex Mono). Contains:

- **Domain string.** The operator-public configuration ID (the domain
  field from the Type3 secret, per `secret-format.md §1`).
- **Canonical serialisation hash.** Algorithm: `sort(key:value pairs by
  key) → concatenate as key=value\n per pair → SHA-256 of UTF-8 bytes`.
  The exact field set is deferred to Epic 3 (installer spec). This spec
  freezes the algorithm.

The hash is reproducibility evidence, not a secret. It allows an operator
to verify that a recovered configuration matches the original install state.
Publishing the domain string and its hash does NOT weaken the Type3 key
material.

### 1.3 Recovery mnemonic

BIP-39-style wordlist, 12 or 24 words, rendered in a monospace block
(IBM Plex Mono). Word count is operator-configurable at install time; 12
words is the default. Each word on its own line or space-separated in
groups of four — implementation choice, normatively format-stable once
chosen.

### 1.4 Co-admin roster

> **PREREQUISITE STATE — Story 1.1 (`secret-format.md §4`, AR-C5
> backup-list data model).** The full AR-C5 schema (signature format,
> wire encoding, canonical field names) is deferred to spec v0.2.0 and
> tracked in `secret-format.md §6.4`. When §4 lands, this section MUST be
> updated with the canonical field names. Until then, placeholder names
> are used: `{pubkey, handle, sequence}`.

A roster of co-administrators for key-rotation and disaster-recovery
succession. Each entry contains:

- `{pubkey}` — public key encoded as a QR code, rendered at approximately
  25 mm square. QR `Alt` text: "Co-admin public key — do not photograph
  without operator consent."
- `{handle}` — human-readable opaque identifier (opaque-handle form,
  e.g. `TP3-XXXXXXXX`).
- `{sequence}` — integer position in the admin roster (1-based).

**Overflow guard.** If the co-admin roster exceeds the physical space
available on one page, entries MUST spill to continuation pages. The
footer MUST carry the updated page count (e.g. "Page 1 of 3"). No entry
MAY be silently truncated.

### 1.5 Handwritten notes area

A ruled, bordered area approximately 6 cm in vertical height. Suitable
for the operator's own handwritten annotations.

For PDF/UA compliance the area MUST be marked as a blank `Annot` or
`Form` element so that screen readers do not announce missing content.

### 1.6 Printable fallback contact channel

Optional; operator-configurable at install time. If provided, rendered in
the footer alongside the page number. The footer MUST NOT carry any tool
name, tool logo, or tool tagline.

## 2. Variants

### 2.1 Page size

| Page size | Dimensions | Default for |
|---|---|---|
| A4 | 297 × 210 mm | All regions not listed below |
| US-Letter | 279 × 216 mm | US, CA, MX, PH, CL, CO, VE, GT operator locales |

The document is designed as a tri-fold: three equal panels when folded
along the long dimension. Both page sizes MUST produce a tri-foldable
layout with panels of equal width.

### 2.2 Locales

Supported locale set (BCP 47 script tags):

| Locale | Script | Text direction |
|--------|--------|----------------|
| `en` | Latin | LTR |
| `ru` | Cyrillic | LTR |
| `fa` | Arabic/Persian | RTL — full mirrored section flow (per ux-spec §11) |
| `zh-Hans` | Han (Simplified Chinese) | LTR |

**Unsupported locale fallback.** If the requested locale is not in the
table above, the installer MUST fall back to `en` and emit a `[warn]`
line on stdout:

```
[warn] locale '<requested>' not supported for recovery letter; falling back to en
```

(Format per `cli-stdout.md` `[warn]` convention.)

**BCP 47 note.** `zh-Hans` MUST be used — not bare `zh`, which is
ambiguous between Simplified and Traditional Chinese. Han glyphs MUST
be rendered from a Han-glyph-only font subset.

### 2.3 Print optimisation

The document is monochrome print-optimised. All information MUST be
conveyed by shape and text — no colour-only distinction is permitted.

## 3. Typography

| Block type | Font |
|---|---|
| Prose (all sections) | Libertinus Serif |
| Monospace (mnemonic, fingerprint, server ID, handle, URL blocks) | IBM Plex Mono |

Per UX-DR25 (ux-design-specification.md §8.3 typography register). No
decorative or display fonts. No system UI fonts.

## 4. Accessibility (PDF/UA per UX-DR22)

Requirements for the rendered PDF:

| PDF/UA element | Requirement |
|---|---|
| Tagged PDF | MUST be set (`MarkInfo` dictionary `Marked: true`) |
| `StructTreeRoot` | MUST be populated |
| `Lang` | MUST match the selected locale BCP 47 code |
| Heading structure | H1 → H2 → H3 reflecting six-section anatomy; no skipped levels |
| QR figures | MUST be `Figure` elements; `Alt` MUST describe purpose WITHOUT leaking the secret URL |
| Handwritten-notes area | MUST be marked blank `Annot` or `Form` element |
| Language spans | Mixed-locale runs (e.g. English labels within a `fa` document) MUST carry explicit `Lang` attribute |
| Role-map | Standard structure types used; custom `RoleMap` only when standard types are insufficient |

## 5. Deterministic rendering invariant

Given identical operator inputs — `{handle, fingerprint, mnemonic,
co-admin roster, locale, paper size}` — the rendered PDF MUST be
byte-stable: two independent renders from the same inputs MUST produce
byte-identical files (same SHA-256 hash).

**Test obligation (normative).** A conforming implementation MUST
demonstrate the invariant by regenerating the recovery letter twice from
identical inputs and asserting SHA-256 hash equality before the letter
is considered accepted.

**Implementation notes (informative; deferred to Epic 3 / Epic 8).**
Achieving byte-stability requires: Typst `--root` path pinning,
deterministic font embedding, timestamp suppression (no `CreationDate` /
`ModDate` in PDF trailer metadata), build-host neutrality (no hostname or
username embedded in PDF metadata). These are implementation concerns for
the Typst pipeline in Epic 3 / Epic 8; the byte-stability invariant itself
is normative and takes effect at v0.1.0.

## 6. PDF/UA field contract

| Field | Required value |
|---|---|
| `Title` | "Recovery Letter" (localised to selected locale) |
| `Lang` | BCP 47 locale code (e.g. `en`, `ru`, `fa`, `zh-Hans`) |
| `MarkInfo.Marked` | `true` |
| `StructTreeRoot` | populated (non-null) |
| Tagged document | PDF 1.7+ tagged document flag set |
| `RoleMap` | standard types preferred; custom map only where standard is insufficient |
| Language spans | per-run `Lang` attribute on text spans in mixed-locale content |
| `Figure` alt text | descriptive; MUST NOT contain the secret URL or any server address |

## 7. Locale notes

- **`fa` RTL.** Full mirrored section flow per ux-design-specification.md
  §11: column order mirrors, margin assignments swap, text direction is
  `rtl`. All six anatomy sections appear in the same reading order; only
  the spatial layout mirrors.
- **`zh-Hans`.** Han-glyph-only font subset; `zh-Hans` BCP 47 script tag
  MUST be used in the PDF `Lang` field.
- **`en` / `ru`.** Standard LTR flow.

**Untranslated placeholders.** Placeholder strings (`TP3-XXXXXXXX`,
`{pubkey}`, `{handle}`, `{sequence}`, BIP-39 word slots) are
language-neutral. They MUST NOT be translated. An optional localised
explanatory line MAY appear immediately beneath each placeholder in the
selected locale.

## 8. Test vectors

Test vectors for the recovery letter artefact — visual acceptance oracles
and rendering round-trip hash assertions — are tracked under the key
`recovery-letter` in `../conformance/vectors/unit.json`.

**Vector population deferred to Epic 3 / Epic 8.** This story adds no
`unit.json` keys.

## 9. Contested Decisions

- **Rejected: embed Typst sources in this story.** Typst sources
  (`spec/ux-tokens/pdf/recovery-letter.typ`), font binaries, and the
  deterministic-build pipeline all belong in Epic 3 (greenfield installer)
  and Epic 8 (operator-tooling polish), where the full build environment
  is specified. Deferred to: Epic 3 / Epic 8.

- **Rejected: JSON-LD signed manifest instead of physical paper artefact.**
  A digital-only manifest does not survive the disaster scenarios (hardware
  failure, power outage, access lockout) that the recovery letter is
  designed for. A physical, hand-annotatable document is the primary
  contract. Deferred to: not applicable — rejected outright for v0.1.x;
  revisit only if operator research at v1.0 demonstrates demand.

## 10. Banned tokens

The following AR-C1 tokens are listed for operator-reference documentation
purposes only. Outside this sentinel block, these tokens MUST NOT appear
in any prose, key, value, alt-text, or comment in this file.

<!-- ban-list-doc: AR-C1 seven-token reference for recovery-letter artefact -->
| Token          | Script         |
|----------------|----------------|
| `proxy`        | Latin          |
| `proxy-server` | Latin          |
| `bypass`       | Latin          |
| `censorship`   | Latin          |
| `прокси`       | Cyrillic       |
| `پروکسی`       | Arabic/Persian |
| `代理`         | Han (CJK)      |
<!-- /ban-list-doc -->
