---
spec_version: 0.1.0-draft
last_updated: 2026-05-14
status: draft
---

> **Normative.** This document defines required behaviour for conforming
> Type3 implementations. Code in `lib/` is illustrative; where this
> document and `lib/` differ, this document wins.

# QR Sticker

Physical sticker art for QR placement in public and semi-public contexts.
The artefact is a pure QR code — no border, no frame, no text, no logo.
(Source: ux-design-specification.md §C5.)

## 1. Anatomy

The sticker contains exactly one element: the QR pattern encoding the
secret URL. Nothing else is permitted.

| Permitted | Forbidden |
|---|---|
| QR code pattern | Border of any kind |
| Required quiet zone (§5) | Frame or surround |
| | Text (label, caption, URL) |
| | Logo or brand mark |
| | Abstract decorative pattern |
| | Generic art surround |

## 2. Sizes

Three canonical breakpoints are defined. Implementations MUST support at
least these three; intermediate sizes MAY be supported.

| Canonical size | Dimensions | Use context |
|---|---|---|
| Small | 25 × 25 mm | Discreet placement |
| Medium | 40 × 40 mm | Standard placement |
| Large | 70 × 70 mm | Counter-visible / high-visibility placement |

The size refers to the full printed sticker area including the quiet zone
(§5). The QR pattern MUST fill the available area after subtracting the
quiet zone (≥ 4 modules on all sides).

## 3. Error-correction level

| Level | Status |
|---|---|
| **M** | **MANDATED** — implementations MUST use level M by default |
| Q | PERMITTED — operator MAY request level Q |
| H | PERMITTED — operator MAY request level H |
| L | MUST NOT — implementations MUST NOT use level L |

Level M provides approximately 15% recovery capacity, sufficient for minor
surface damage on a sticker without increasing module density to the point
where small-format stickers become unreadable. Level L is prohibited because
it provides only 7% recovery, making the sticker vulnerable to even minor
abrasion.

## 4. "Unmarked art" — normative definition

An artefact qualifies as **unmarked art** if and only if it contains none
of the following:

1. Frame — any drawn border around the QR pattern.
2. Border — any stroke or fill enclosing the sticker edge.
3. Logo — any brand symbol, glyph, or pictogram.
4. Text overlay — any character, digit, or punctuation overlaid on or
   adjacent to the QR modules.
5. Abstract pattern — any decorative geometric or freeform shape not part
   of the QR structure.
6. Generic art surround — any illustrative or photographic background
   behind the QR modules.

This six-item enumeration is exhaustive. Absence of all six items is
sufficient and necessary for the artefact to qualify as unmarked art.
The rule is sourced from ux-design-specification.md §C5 ("pure QR code,
no border, no frame, no text, no logo") and AR-C1 visual-discipline
constraints.

## 5. Print specification

- **Quiet zone.** MUST be ≥ 4 QR modules on all four sides. (ISO/IEC
  18004 requirement; scanners rely on it for pattern detection.)
- **Colour.** Black-on-white MANDATED as the default. Coloured-on-white
  variants are permitted provided the module-to-background contrast ratio
  is ≥ 4.5:1 (per UX-DR19, ux-design-specification.md §WCAG).
- **Background.** Transparent background MUST NOT be used; stickers are
  placed on varied surfaces and module contrast must be self-contained.

### 5.1 Output format

| Format | Status |
|---|---|
| SVG | **Default** — implementation MUST produce SVG unless `--format` is specified |
| PNG | Produced when operator passes `--format=png` flag |

SVG output MUST use integer-pixel viewBox with no fractional coordinates.
PNG output MUST be at a resolution sufficient to ensure each module is
rendered at the correct physical size when printed at target DPI (e.g. 300
or 600 DPI for home printing).

### 5.2 Module-density guards

Module size (physical width of one QR module in mm) is determined by:

```
module_size_mm = sticker_size_mm / (QR_version_module_count + 2 × quiet_zone_modules)
```

Generation MUST be rejected with an error if:

| Sticker size | Threshold | Condition |
|---|---|---|
| 25 × 25 mm | < 0.30 mm per module | Reject |
| 35 × 35 mm (Handoff Card placement context) | < 0.35 mm per module | Reject |

When rejected, the error message MUST instruct the operator to use a larger
size:

```
[err] module size <N>mm at 25mm sticker would be below minimum (0.30mm); use --size=40mm or larger
```

(Format per `cli-stdout.md` `[err]` convention.)

## 6. Test vectors

Test vectors for the QR sticker artefact (module-density assertions,
format-compliance checks) are tracked under the key `qr-sticker` in
`../conformance/vectors/unit.json`.

**Vector population deferred to Epic 3 / Epic 8.** This story adds no
`unit.json` keys.

## 7. Contested Decisions

- **Rejected: embed Typst sources in this story.** Typst sources
  (`spec/ux-tokens/pdf/qr-sticker.typ`) and the rendering pipeline belong
  in Epic 3 (greenfield installer) and Epic 8 (operator-tooling polish).
  Deferred to: Epic 3 / Epic 8.

- **Rejected: decorative frame or abstract art surround.** Any frame,
  border, or art surround violates the §1 anatomy requirement (pure QR)
  and the §4 unmarked-art definition. This alternative is rejected
  outright for all v0.1.x versions and is not deferred.

## 8. Banned tokens

The following AR-C1 tokens are listed for operator-reference documentation
purposes only. Outside this sentinel block, these tokens MUST NOT appear
in any prose, key, value, alt-text, or comment in this file.

<!-- ban-list-doc: AR-C1 seven-token reference for qr-sticker artefact -->
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
