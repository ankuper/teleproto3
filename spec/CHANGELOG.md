# spec/ Changelog

All notable changes to the Type3 specification. Follows [SemVer](https://semver.org/)
with prefixed tags (`spec-v0.1.0`, `spec-v1.0.0`, ...).

## [Unreleased]

### Added
- Initial scaffold: reading order, RFC 2119 discipline, banner conventions.
- A-004 (2026-04-25): Length-silence. Spec imposes no upper bound on
  `domain_len`, `host_len`, or `path_len` beyond the §1 `domain_len ≥ 1`
  constraint. §6.3 (RFC 1035 253-octet ceiling) and §6.8 (length-prefixed
  path) record the rejected alternatives. Companion to A-003.
- A-003 (2026-04-25): Destination structure inside `domain` field. New §1.1
  defines `host[/path]` split on first `0x2f`. New §2.1 MUST rule: reject
  `domain` beginning with `/` (no host) as `MALFORMED`. New §3 producer rules:
  construct `domain = host || path` by octet concatenation; producers MUST NOT
  emit a `host` containing `0x2f` or an empty `host`. §5.2 result schema
  extended with `result.host` and `result.path` (additive; `result.domain`
  preserved); consumer guidance promoted from table cell to body prose. §5.4
  vector inventory grows from 11 to 16 vectors: `well-formed-host-and-path`,
  `well-formed-host-and-deep-path`, `well-formed-idn-host-and-path`,
  `well-formed-host-and-trailing-slash`, `malformed-leading-slash`. §6.7
  records rejected Variant B (separate URL `?path=` parameter). §6.8 records
  rejected Variant C (length-prefixed path). New §4.3 forward-compat
  constraint: future version markers MUST NOT equal `0x2f` (preserves §1.1
  split semantics). New §1.1 trailing-slash clarifier: `path = "/"` only is
  well-formed. Spec stays length-silent per A-004.

## [spec-v0.1.0-draft] — 2026-04-24

Draft initialisation. Not a published tag. All normative sections are
skeletons; first real content lands in the spec-v0.1.0 drafting epic.
