---
spec_version: 0.1.0-draft
last_updated: 2026-05-14
status: draft
---

> **Normative.** This document defines required behaviour for conforming
> Type3 implementations. Code in `lib/` is illustrative; where this
> document and `lib/` differ, this document wins.

# Handoff Card

Pocket-sized printable card a Giver can hand to a Recipient IRL containing
an unmarked QR of the secret, space for the Giver's handwritten note,
nothing else visible. (Source: ux-design-specification.md §C4.)

## 1. Anatomy

The card has two faces. Each face has normative MUST / MUST NOT
requirements.

### 1.1 Front face

- **QR code.** Unmarked QR code, approximately 35 mm square, centred on
  the face. No border, no frame, no surround.
- **Contrast.** The QR module contrast MUST be ≥ 4.5:1 (black-on-white
  or equivalent) for scan reliability. Ruled-line contrast on the front
  face (if any) MUST be in the range 1.5:1 to 3:1 — light enough not to
  obscure the QR, visible enough to show the boundary area.
- **No text.** No label, no URL, no caption MUST appear on the front face.
- **No logo.** No tool name, tool logo, tagline, or brand mark MUST appear
  on the front face.

### 1.2 Back face

- **Ruled space.** Horizontal ruled lines for handwritten note. Ruling
  contrast: 1.5:1 to 3:1 (light grey on white — guides the hand without
  overpowering handwriting).
- **No pre-printed text.** No label, no heading, no instructions, no prompt
  text MUST be pre-printed on the back face. The Giver's handwriting is the
  only personalisation.

## 2. Variants

| Variant | Dimensions | Notes |
|---|---|---|
| Credit-card | 85 × 54 mm | **Default** |
| Business-card | 89 × 51 mm | Common alternative to credit-card |
| A6 | 148 × 105 mm | Larger; suitable for longer co-admin rosters |
| Postcard | 148 × 100 mm | Regional postcard standard |

Typography, margins, and QR sizing rules are identical across all variants.
Implementors MUST NOT introduce variant-specific layout logic beyond
scaling the QR to maintain the approximately 35 mm target square.

## 3. Paper specification

- **Weight.** 80 gsm. Designed for home printing. Per ux-design-specification.md
  §C4 verbatim: "Designed for home printing on 80 gsm."
- **Handling resilience.** The card MUST survive normal handling: pocket
  carry, wallet carry, and casual folding.
- **Fold testing.** Fold-stress testing (repeated crease-fold endurance) is
  NOT REQUIRED for credit-card and business-card sized variants. These sizes
  are pocket-carried, not folded repeatedly. Fold-stress requirements are
  reserved for future artefacts (e.g. the recovery letter, which is
  tri-foldable). 250 gsm card stock is explicitly OUT of contract for this
  artefact.

### 3.1 Crop marks

Home-printable output files MUST include hairline crop marks outside the
bleed zone to allow the operator to trim accurately. Crop marks:

- Line weight: hairline (0.25 pt or thinner).
- Position: 3–5 mm outside the finished edge, in the bleed zone.
- Style: standard corner-mark crosshairs; no full-bleed border.

## 4. Typography register

| Block type | Font |
|---|---|
| Any incidental text (variant labels, print guides) | Libertinus Serif |
| Monospace blocks (future extension only) | IBM Plex Mono |

Per UX-DR25 (ux-design-specification.md §8.3). The card itself carries no
pre-printed prose in the default layout; typography applies if variant
labels or print-guide text is included in the template file.

## 5. Visual discipline

The following elements MUST NOT appear on either face of the card in any
variant:

| Forbidden element | Rationale |
|---|---|
| Logo of any kind | AR-C1 + §C4 unmarked-art rule |
| Tool name | AR-C1 + §C4 |
| Tagline | AR-C1 + §C4 |
| URL text beside the QR | §C4 explicit prohibition |
| Decorative frame or border around QR | §C4 unmarked-art rule |
| Abstract pattern surround | §C4 unmarked-art rule |

The Giver's handwriting on the back face is the ONLY personalisation. No
other personalisation mechanism is defined at v0.1.0.

## 6. Test vectors

Test vectors for the handoff-card artefact (visual acceptance oracles,
dimension-compliance assertions) are tracked under the key `handoff-card`
in `../conformance/vectors/unit.json`.

**Vector population deferred to Epic 3 / Epic 8.** This story adds no
`unit.json` keys.

## 7. Contested Decisions

- **Rejected: embed Typst sources in this story.** Typst sources
  (`spec/ux-tokens/pdf/handoff-card.typ`), font binaries, and the
  deterministic-build pipeline belong in Epic 3 (greenfield installer) and
  Epic 8 (operator-tooling polish). Deferred to: Epic 3 / Epic 8.

- **Rejected: operator-configurable cover art or decorative frame.**
  Any frame, border, abstract pattern, or logo surround on the front face
  violates the §1.1 anatomy requirement (unmarked QR) and the §C4
  unmarked-art rule. No operator-configuration option for front-face
  decoration is permitted at v0.1.0 or any future v0.1.x version.

## 8. Banned tokens

The following AR-C1 tokens are listed for operator-reference documentation
purposes only. Outside this sentinel block, these tokens MUST NOT appear
in any prose, key, value, alt-text, or comment in this file.

<!-- ban-list-doc: AR-C1 seven-token reference for handoff-card artefact -->
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
