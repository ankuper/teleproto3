# spec/ Changelog

All notable changes to the Type3 specification. Follows [SemVer](https://semver.org/)
with prefixed tags (`spec-v0.1.0`, `spec-v1.0.0`, ...).

## [Unreleased]

### Added
- Initial scaffold: reading order, RFC 2119 discipline, banner conventions.
- A-003 (2026-04-25, architect amendment): Destination structure inside
  `domain` field. New §1.1 defines `host[/path]` split on first `0x2f`. New
  §2.1 MUST rule: reject `domain` beginning with `/` (no host) as `MALFORMED`.
  New §3 producer rule: construct `domain = host || path` by octet
  concatenation. §5.2 result schema extended with `result.host` and
  `result.path` (additive; `result.domain` preserved). §5.4 vector inventory
  grows from 11 to 14 vectors: `well-formed-host-and-path`,
  `well-formed-host-and-deep-path`, `malformed-leading-slash`. §6.7 records
  rejected Variant B (separate URL `?path=` parameter). §6.8 records rejected
  Variant C (length-prefixed path).
- A-004 (2026-04-25, companion to A-003): Length-silence. Spec imposes no
  upper bound on `domain_len`, `host_len`, or `path_len` beyond the §1
  `domain_len ≥ 1` constraint. §6.3 (RFC 1035 253-octet ceiling) and §6.8
  (length-prefixed path) record the rejected alternatives.
- A-003 review fold-in (2026-04-25, post-amendment adversarial re-review):
  12 patches and 2 resolved Decisions applied on top of A-003. §1.1 host
  non-emptiness invariant (`host_len ≥ 1`) + trailing-slash clarifier
  (`path = "/"` is well-formed) + empty-path-vs-trailing-slash distinction
  rule. §2.1 leading-`/` MUST simplified to `D[0] == 0x2f`. §2.1 UTF-8
  rejection wording aligned with RFC 3629 §3 (CESU-8/WTF-8 surrogate
  encodings). §3 producer rules expanded: producers MUST NOT emit a `host`
  containing `0x2f` or an empty `host`. §5.2 consumer guidance hoisted from
  table cell to body prose. New §4.3: future version markers MUST NOT equal
  `0x2f` (preserves §1.1 split semantics). §5.4 vector inventory grows
  14 → 16: `well-formed-idn-host-and-path` (P-8 — IDN host with path) and
  `well-formed-host-and-trailing-slash` (P-14 — `path = "/"`). §6.7 ban-list
  sentinel pair wraps the literal Telegram deeplink reference (sentinel
  grammar pinned in §6.7 inline comment for Story 1.4 linter contract).

## [spec-v0.1.0-draft] — 2026-04-24

Draft initialisation. Not a published tag. All normative sections are
skeletons; first real content lands in the spec-v0.1.0 drafting epic.
