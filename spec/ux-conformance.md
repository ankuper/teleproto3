---
spec_version: 0.1.0-draft
last_updated: 2026-05-14
status: draft
---

> **Normative.** This document defines required behaviour for conforming
> Type3 implementations. Code in `lib/` is illustrative; where this
> document and `lib/` differ, this document wins.

# UX Conformance

Client UI requirements for Type3. A client claiming Full compliance MUST
pass every clause in §§1–9. Sections are not renumbered after publication;
downstream stories (1.6 / 2.x / 4.x / 5.x) cite section numbers verbatim.

## 1. Host-inheritance contract

Conforming Type3 clients render the C1 indicator and its companion glyphs
inside a host application (Telegram Desktop / Android / iOS, or a third-
party fork). Visual identity is **inherited** from the host: Type3 ships
only the semantic state machine and the geometric/motion contract, never
brand-level pixel values.

Five host tokens are frozen by this spec and MUST be readable by the C1
adapter at render time:

| Token            | Purpose                                       |
|------------------|-----------------------------------------------|
| `accent`         | Host brand accent (e.g. Telegram blue)        |
| `bg`             | Host application background                   |
| `fg`             | Host foreground / body text                   |
| `corner-radius`  | Host corner-radius scalar (px or pt)          |
| `base-font`      | Host base typography stack                    |

Five token-kit categories are scaffolded under `spec/ux-tokens/`. Each
category owns one normative schema plus at least one reference instance:

- `geometry/` — SVG subset, ring geometry, RTL mirroring flag.
- `motion/`   — JSON keyframe schema (§2).
- `color/`    — palette schema; only `accent.slate.neutral` is reference-
  filled in v0.1.0. Per-platform palettes are Epic-2/4/5 scope.
- `typography/` — host-typography inheritance contract.
- `pdf/`      — Typst report contract for stories 1.5 / Epic 5.

[Source: UX Design Specification §Design System Foundation §1–§2.]

## 2. Motion JSON normative schema

Animation is described declaratively in JSON keyframe form. A motion file
under `spec/ux-tokens/motion/` MUST validate against the schema below.
Adapters compile the JSON to host-native primitives (Lottie / Core
Animation / `ObjectAnimator` / Qt `QPropertyAnimation`).

```json
{
  "duration_ms": 1200,
  "easing": "ease-in-out",
  "loop": true,
  "direction": "ltr",
  "keyframes": [
    { "t": 0.0, "scale": 1.0, "opacity": 1.0,
      "stroke_width": 2.0, "rotation_deg": 0 },
    { "t": 1.0, "scale": 1.0, "opacity": 1.0,
      "stroke_width": 2.0, "rotation_deg": 360 }
  ]
}
```

Required top-level fields: `duration_ms` (integer ms), `easing`
(`linear|ease-in|ease-out|ease-in-out|cubic-bezier(a,b,c,d)`),
`loop` (bool), `direction` (`ltr|rtl|invariant`), `keyframes` (array).
Required per-keyframe fields: `t` (∈ `[0.0, 1.0]`), `scale`, `opacity`,
`stroke_width`, `rotation_deg`. Reference fixture:
`spec/ux-tokens/motion/connecting-rotate.json`.

[Source: UX Design Specification §Design System Foundation §3; UX-DR3.]

## 3. SVG subset + RTL mirroring

C1 ring and companion glyphs are authored as SVG conforming to a
restricted subset:

- Permitted path commands: `M`, `L`, `C`, `A`, `Z`. No `Q`, `T`, `S`,
  `H`, `V`.
- Permitted elements: `<svg>`, `<path>`, `<g>`. No `<defs>` (except a
  `<path>` reused via `<use>` within the same file), no `<filter>`, no
  `<linearGradient>`, no `<radialGradient>`, no `<image>`, no `<style>`
  with selectors, no `<script>`.
- No CSS animation. Motion is supplied by the §2 JSON schema only.

RTL mirroring rule. Every glyph file declares `direction: ltr | rtl |
invariant` (companion JSON sibling, e.g. `ring-connecting.svg` →
`ring-connecting.direction.json`). At render time:

- `invariant` glyphs (e.g. the rotating connecting ring) are NEVER
  mirrored.
- `ltr` glyphs are mirrored to `rtl` for `fa` locale (and any other
  RTL locale onboarded later).
- `rtl` glyphs are NEVER mirrored further.

Reference fixture: `spec/ux-tokens/geometry/ring-connecting.svg`
(direction: invariant).

[Source: UX Design Specification §Design System Foundation §4; UX-DR4;
§J1 RTL contract.]

## 4. Three-tier parity

Pixel parity across client forks is enforced via three tiers measured on
adapter snapshot tests. The tier triggers automatic CI behaviour:

| Tier | Δ pixel range | CI behaviour                                                      |
|------|---------------|-------------------------------------------------------------------|
| 1    | ≤ 2 %         | Strict — pass.                                                    |
| 2    | > 2 %, ≤ 5 %  | Drift — warning; mandatory entry in `drift-log.md` with rationale.|
| 3    | > 10 %        | Halt — release-blocker; merge is rejected.                        |

The gap `> 5 %, ≤ 10 %` is a hard warning (CI still passes) requiring
a justification but no `drift-log.md` entry; > 10 % is the unambiguous
release-blocker.

`spec/ux-tokens/conformance/drift-log.md` is an append-only journal.
Each entry MUST cite: snapshot fixture name, observed Δ%, adapter
(tdesktop / Android / iOS), justification, expiry date.

[Source: UX Design Specification §Design System Foundation §6; UX-DR6.]

## 5. Per-platform adapter-doc skeleton

Each client fork owns a Markdown adapter document describing how Type3
tokens become host-native primitives. The document MUST contain five
sections, in order:

1. **Token input** — list of `spec/ux-tokens/**` inputs consumed and
   their schema version pin.
2. **Platform primitive output** — list of host APIs produced (e.g.
   tdesktop: `Ui::RoundButton`; Android: `MaterialButton`; iOS:
   `UIButton.Configuration`).
3. **Worked example** — one full token → primitive trace (preferably the
   C1 connecting state) including code.
4. **Explicit non-goals** — fork-specific simplifications. Examples:
   "Android does not render the dashed-ring unverified variant in v0.1.0
   — falls back to solid amber."
5. **Known drift** — tier-2 entries from `drift-log.md` that apply to
   this adapter, with expiry dates.

Adapter docs live alongside the fork (tdesktop, Android, iOS), not
inside `teleproto3/`.

[Source: UX Design Specification §Design System Foundation §5; UX-DR5.]

## 6. WCAG 2.1 AA gates

Conforming Type3 implementations MUST meet WCAG 2.1 Level AA contrast
under both light and dark host themes for every C1 base state and every
companion glyph state.

- **Text contrast** — WCAG 2.1 SC 1.4.3: minimum 4.5:1 for normal text;
  minimum 3:1 for large text (≥ 18 pt, or ≥ 14 pt bold). Applies to
  the textual label rendered alongside C1 (e.g. "Connected", "Degraded
  connection").
- **Graphical contrast** — WCAG 2.1 SC 1.4.11: minimum 3:1 for the C1
  ring stroke and companion glyph fill against the host-inherited
  background token. The ring/glyph is a UI component conveying state;
  4.5:1 is NOT required and would force higher-luminance amber that
  breaks the "calm, no alarm" semantic.

Both ratios are codified in `spec/assets/contrast-tokens.yaml`. The
`spec-a11y-strings.yml` CI workflow asserts every C1 state has both a
text-pair entry and a graphical-pair entry, against both `host.bg.light`
and `host.bg.dark`.

[Source: UX Design Specification §Responsive Design & Accessibility
§Accessibility Strategy; AR-G2; WCAG 2.1 SC 1.4.3 + SC 1.4.11.]

## 7. Ban-list enforcement

<!-- ban-list-doc: the seven literal tokens that may not appear in any
     normative user-facing string. Case-insensitive substring match. -->

| Token          | Script         |
|----------------|----------------|
| `proxy`        | Latin          |
| `proxy-server` | Latin          |
| `bypass`       | Latin          |
| `censorship`   | Latin          |
| `прокси`       | Cyrillic       |
| `پروکسی`       | Arabic/Persian |
| `代理`          | Han (CJK)      |

<!-- /ban-list-doc -->

The list applies to every locale simultaneously: a string in any locale
containing a literal from any other script still fails the lint. Match
is case-insensitive substring (NOT word-boundary); operators rephrase
rather than rely on lexical edges.
The `spec-uicopy-banlist.yml` CI workflow scans every `.md`, `.yaml`,
`.yml`, `.json` file under `spec/` and skips text inside
`<!-- ban-list-doc: ... -->` HTML-comment sentinels.

[Source: style-guide §8 AR-C1; UX-DR24.]

## 8. Screen-reader metadata schema

Each normative entry in `ux-strings.yaml` MUST carry a screen-reader
metadata block:

```yaml
c1.connecting.label:
  en: "Connecting"
  ru: "Подключение"
  fa: "در حال اتصال"
  zh: "正在连接"
  pronunciation_context: "verb_progressive"
  max_utterance_length_ms: 900
  interruption_behavior: "queue"
  verified: false
```

Field semantics:

- `pronunciation_context` — disambiguator for homographs and TTS hint.
  Values: `noun | verb_progressive | verb_imperative | adjective |
  proper_noun | other`.
- `max_utterance_length_ms` — adapter target for utterance pacing.
  Adapters that exceed this MUST truncate or compress phonetically.
- `interruption_behavior` — `queue | preempt | drop`. Default `queue`.
  Tier-3 toasts are `preempt`; status labels are `queue`.
- `verified` — `false` until a native reviewer has audited the
  translation for naturalness and ban-list cleanliness. Compliance
  level Full flags `verified: false` as warning, not failure; missing
  metadata is a hard failure.

Keys MUST use dotted segmentation (`c1.idle.label`, NOT
`c1_idle_label`). The dotted-key convention is normative across the
spec for all token / metadata namespaces.

[Source: UX Design Specification §Design System Foundation §10; UX-DR20.]

## 9. Silent-downgrade detection contract (UX-DR34)

Type3 retry-tier transitions are surfaced to the user via the C1
indicator. The runtime exposes the current retry tier as a typed enum
declared in `libteleproto3` (forward-referenced from story 1.6):

```c
typedef enum {
  T3_RETRY_OK,
  T3_RETRY_TIER1,
  T3_RETRY_TIER2,
  T3_RETRY_TIER3
} t3_retry_state_t;
```

Adapters MUST map enum values to C1 visual states verbatim:

| Enum value         | C1 visual state             | Companion behaviour                              |
|--------------------|-----------------------------|--------------------------------------------------|
| `T3_RETRY_OK`      | `connected-verified` (solid) | None.                                            |
| `T3_RETRY_TIER1`   | `connected-unverified` (dashed ring) | None.                                    |
| `T3_RETRY_TIER2`   | `degraded-t12` (amber)      | None (no toast; status-bar update only).         |
| `T3_RETRY_TIER3`   | `degraded-t3` (amber + toast) | One-shot toast (`ui.tier3-toast`) with action `ui.tier3-action-switch`. |

Adapters MUST NOT invent additional enum names (e.g.
`T3_RETRY_DEGRADED`). Adapters MUST NOT collapse `TIER1` and `TIER2`
into a single visual state — `TIER1` is verification-only drift,
`TIER2` is timing-quality drift, and the spec separates them on the C1
indicator to make the "silent downgrade" diagnosable by the user.

Transition latency from enum write to visual update is target ≤ 16 ms
on adapter benchmark; this target is aspirational and subject to a
deferred re-baseline (see deferred item 1-3-DEF? / story 1.6).

[Source: UX Design Specification §Defining Core Experience §7.8
silent-downgrade contract; UX-DR34; style-guide §9 libteleproto3 ABI
surface (`t3_retry_state_t` enum freeze).]
