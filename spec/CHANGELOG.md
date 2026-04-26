# spec/ Changelog

All notable changes to the Type3 specification. Follows [SemVer](https://semver.org/)
with prefixed tags (`spec-v0.1.0`, `spec-v1.0.0`, ...).

## [Unreleased]

### Added
- Initial scaffold: reading order, RFC 2119 discipline, banner conventions.
- A-003 (2026-04-25): Destination structure inside `domain` field. New §1.1
  defines `host[/path]` split on first `0x2f`. New §2.1 MUST rule: reject
  `domain` beginning with `/` (no host) as `MALFORMED`. New §3 producer rule:
  construct `domain = host || path` by octet concatenation. §5.2 result schema
  extended with `result.host` and `result.path` (additive; `result.domain`
  preserved). §5.4 vector inventory grows from 11 to 14 vectors:
  `well-formed-host-and-path`, `well-formed-host-and-deep-path`,
  `malformed-leading-slash`. §6.7 records rejected Variant B (separate URL
  `?path=` parameter). §6.8 records rejected Variant C (length-prefixed path).
  Spec stays length-silent per A-004; §6.3 unchanged.

## [spec-v0.1.0-draft] — 2026-04-24

Draft initialisation. Not a published tag. All normative sections are
skeletons; first real content lands in the spec-v0.1.0 drafting epic.
