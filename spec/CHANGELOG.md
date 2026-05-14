# spec/ Changelog

All notable changes to the Type3 specification. Follows [SemVer](https://semver.org/)
with prefixed tags (`spec-v0.1.0`, `spec-v1.0.0`, ...).

## [Unreleased]

### Added
- A-011 (2026-05-14, Story 1-5a): operator-artefact spec landing.
  Added `spec/recovery-letter.md` (AR-C6 / UX-DR9 — six-section A4 / US-Letter
  tri-foldable PDF contract; PDF/UA per UX-DR22; locales en/ru/fa/zh-Hans;
  deterministic-render invariant; Typst implementation deferred to Epic 3 / 8).
  Added `spec/handoff-card.md` (UX-DR10 — credit-card / A6 / postcard / business-card
  variants on 80 gsm; AR-C1 visual discipline; crop marks for home printing).
  Added `spec/qr-sticker.md` (UX-DR11 — pure-QR; sizes 25–70 mm; error-correction
  level M; SVG default + PNG via `--format=png`; module-density guards).
  Added `spec/cli-stdout.md` (UX-DR12 — `[ok]/[warn]/[err]/[info]` prefixes;
  two-space indent; TTY-vs-pipe via stream-is-interactive-terminal test;
  `NO_COLOR` + `TERM=dumb` honoured; structured-output `--output=` reserved per
  COP-7, schema deferred to Epic 8). Reading order in `spec/README.md` extended
  with items 9–12; `conformance-procedure.md` renumbered 9 → 13. Re-does the
  content of Story 1-5's 2026-04-28 close-out, which was demoted `done → review`
  on 2026-05-13 per the sprint-wide trust audit (`_bmad-output/process/
  change-type-checklists.md` §Worked Example C1) when its four `[NEW]` File List
  entries were existence-checked as fictional.
- E-002 (2026-05-13, Story 1-3a): `spec/anti-probe.md` §8 Timing Invariants
  normative landing in TOST + Spearman + Bonferroni formulation. Supersedes
  the legacy Mann-Whitney U phrasing in `_bmad-output/planning-artifacts/epics.md`
  AR-C2 (style-guide §14.8 anti-pattern). N_required gate derived from pilot σ̂
  in `conformance/baselines/lib-v0.1.x/ar-c2-pilot.yaml`. Bucket edges aligned
  with Story 1-10 fuzz harness (5 buckets: [0,63]/[64,255]/[256,1023]/[1024,4095]/
  [4096,16383]). Cross-refs: `lib/fuzz/analyse.py` (1-10, already aligned),
  `conformance/gates/timing_invariant.py` (7-2 — Mann-Whitney → TOST + Bonferroni
  rewrite tracked under Story 7-2 D1 resolution, not this story).
- A-010 (2026-05-12, Story 7-1 code-review D2): A-007 consumer-side split
  rule. §5.2 result-schema gains optional `result.query` and
  `result.fragment` fields. Consumers parsing a domain with a literal `?`
  / `#` octet (a producer-side A-007 violation) MUST route the offending
  suffix into `result.query` / `result.fragment` so that `result.host`
  stays DNS-clean and `result.path` stays URL-clean by construction. The
  split is informational on the consumer side; the producer-side
  MUST-NOT-emit rule from A-007 is unchanged. Schema strictness clause
  amended to make optional fields verifier-compared only when the vector
  declares them. Driven by code-review finding that `result.host =
  "example.invalid?q=1"` is HTTP-injection-adjacent surface.
- A-009 (2026-05-03, Story 1-11): `T3_CSPRNG_BACKEND` first-class enum value in
  `lib/include/t3_platform.h`. Default value `none`. Producer-side RNG selection
  is an implementation detail and MUST NOT alter KDF output or secret-format wire
  encoding. A-009 is orthogonal to the secret-format spec; it constrains the
  platform-layer API surface only. Cross-ref: `lib/include/t3_platform.h`.

  ### Test Vectors (Story 7-1)
  New section `kdf-cross-csprng:` in `conformance/vectors/unit.json` — 5 vectors
  confirming that KDF output is byte-identical across all CSPRNG backend values:
  `kdf-csprng-backend-none`, `kdf-csprng-backend-linux-getrandom`,
  `kdf-csprng-backend-macos-secrandom`, `kdf-csprng-backend-windows-bcrypt`,
  `kdf-csprng-backend-invalid-value` (negative: wire-class `INVALID_ARG` with
  `detail.lib_code="INVALID_CONFIG"`; `t3_init()` MUST return
  `T3_ERR_INVALID_CONFIG` for unrecognised backend string).
  All 5 vectors marked `expected_status: pending-implementation` pending a Linux
  reference-impl run (follow-up 7-1-A).

- A-007 (2026-04-28, Story 1-1 round-9): Producer-side normative `?` / `#`
  rejection. Producers MUST NOT emit a secret whose `host` or `path` contains a
  literal `0x3f` (`?`) or `0x23` (`#`) octet. These characters create ambiguity
  with HTTP query-string / fragment semantics when the `host[/path]` pair is
  embedded verbatim in HTTP request targets. Consumers MUST parse gracefully
  (Postel principle — lenient parsing, strict producing). URL-encoded forms
  (`%3F`, `%23`) are unaffected; the encoding is a consumer-side concern.
  Cross-ref: `secret-format.md §2.1 producer-rules`.

  ### Test Vectors (Story 7-1)
  Five vectors added to `conformance/vectors/unit.json` `secret-format:` array:
  `a007-host-with-query` (host `?` — consumer accepts, routes suffix to `result.query` per A-010),
  `a007-host-with-fragment` (host `#` — consumer accepts, routes suffix to `result.fragment` per A-010),
  `a007-path-with-query` (path `?` — consumer accepts, routes suffix to `result.query`),
  `a007-path-with-fragment` (path `#` — consumer accepts, routes suffix to `result.fragment`),
  `a007-path-with-percent-encoded-query` (path `%3F` — consumer accepts as passthrough; no split).
  Summary: A-007 producer-side rule unchanged; consumer-side split lands as A-010.
  All 5 consumer vectors pass (`ok: true`); `result.host` and `result.path` remain
  byte-clean by construction.

- W-001 (2026-04-27, Story 1-2 Round-8 close-out): `spec/wire-format.md`
  §4 obfuscated stream lifted from ERRATA-deferred to normative.
  §4.1 Quick reference (informative summary + 4 top-level MUSTs:
  no-derivation-from-IP, continuous-CTR-counter, SHOULD-zeroise-secret,
  per-session-uniqueness). §4.2 Normative derivation (full SHA-256
  formulae inline; `read_key`/`read_iv`/`write_key`/`write_iv` slices
  named verbatim against `random_header`; magic-tag admission gate
  `{0xdddddddd, 0xeeeeeeee, 0xefefefef}`). §4.3 Idle session — zero
  obfuscated payload after Session Header is WELL-FORMED. New ERRATA
  entry E-001 records v1 KDF audit trail (citation:
  `mtproxy3-legacy/src/net-tcp-rpc-ext-server.c:1407–1467`).
- W-002 (2026-04-27, Story 1-2 Round-8 close-out, batched edits to
  `spec/wire-format.md`): §1 hard-pins HTTP/1.1 (RFC 8441 / RFC 9220
  RESERVED-for-v0.2 + frontend MUST-NOT-downgrade); §1 GET-target
  construction rule pins structured `(host, path)` over unsplit
  `domain` (Round-8 B3 / R8-1); §1 `Host:` raised to canonical RFC
  6455 list (R8-13); §1 `Sec-WebSocket-Protocol` MUST-NOT-echo (D4);
  §1 `mtproxy3-architecture.md` reference wrapped in ban-list-allow
  sentinel (R8-3); §2 flat `MALFORMED` for all framing violations
  with optional MAY-log-distinguish line (D2); §2 explicit
  EOF-during-fragmentation contract (R8-10); §3 sender vs receiver
  duty split in registry table (R8-7); §3 `0xFF` cross-reference to
  `secret-format.md` §1 sentinel namespace (R8-11); §3 `RESERVED`
  caps demoted from RFC 2119 (R8-15); §3 `flags = 0x0001` example
  clarified as MALFORMED-at-v0.1.0 (R8-16); §3 (`command_type`,
  `version`) error class table added — future-recognised slot maps
  to `UNSUPPORTED_VERSION` (D3); §5 silent-close terminology
  consolidated; §5 timing pre-Story-1.3 bridge text inlined (M1);
  frontmatter `last_updated: 2026-04-27` + `amendment_log`
  populated. Conformance vectors: `session-header` array 11 → 16
  entries (`fragmented-header-incomplete-eof`,
  `fragmented-header-zero-frames`, `zero-length-payload-idle-session`,
  `unknown-command-type-mid-range`, `unknown-command-type-future-version`)
  + 2 renames (`flags-le-byte-order` → `flags-low-byte-set-rejected`,
  `flags-le-high-byte` → `flags-high-byte-set-rejected`); new
  `obfuscated-handshake` section with 5 KDF KAT vectors
  (`kdf-tag-eeeeeeee`, `kdf-tag-dddddddd`, `kdf-tag-efefefef`,
  `kdf-tag-mismatch`, `kdf-symmetry-roundtrip`).
- A-006 (2026-04-27, Round-8 review fold-in): §2.1 MALFORMED rule 6
  added — reject any octet in `0x00..0x1F` or `0x7F` (ASCII C0 control
  characters and DEL). Rationale: the wire layer (`spec/wire-format.md`
  §1) embeds parsed `host[/path]` verbatim into HTTP request lines; an
  embedded CR/LF/NUL would create a request-smuggling injection
  surface. Fires after rule 5 since the targeted octets are themselves
  valid UTF-8 single-byte scalars. §3.1 valid-`t3_secret_t` predicate
  extended to include rule 6. §1.1 frontmatter `amendment_log` records
  A-006.

  ### Test Vectors (Story 7-1)
  Task 3 — 5 thematic vectors in `secret-format:` array:
  `a006-lf-reject` (0x0A mid-host, rule=control-byte),
  `a006-cr-reject` (0x0D mid-host, rule=control-byte),
  `a006-dot-path-passthrough` (`/.` — no normalisation),
  `a006-dot-dot-path-passthrough` (`/..` — no parent-dir rejection),
  `a006-utf8-overlong-c0af` (`C0 AF` mid-host, rule=utf8-overlong).
  Task 10 — byte-resolution sweep: `a006-ctrl-0x00` through
  `a006-ctrl-0x1f` + `a006-ctrl-0x7f` (33 vectors, one per C0/DEL byte,
  all at non-zero offset).

- Round-8 review vectors (2026-04-27, 6 new entries, §5.4 inventory
  25 → 31): `malformed-domain-control-byte` (A-006 rule 6 reject),
  `well-formed-host-and-double-slash-path` (P-8 — `//x`),
  `well-formed-host-and-dot-path` (P-8 — `/.`),
  `well-formed-host-and-dot-dot-path` (P-8 — `/..`),
  `malformed-domain-utf8-mid-host` (P-15 — `C0 AF` mid-host),
  `well-formed-domain-511-host-and-trailing-slash` (P-16 — A-005
  ceiling × trailing-slash).
- Round-8 review editorial improvements (2026-04-27, post-Round-7
  closure code review): §1 SIZE_MAX prose rephrased (rule-1
  precondition + rule-3 rejection chain made explicit). §2.1 rule 5
  enumeration extended (4-byte overlongs `F0 80 80 80`; out-of-range
  scalars >U+10FFFF e.g. `F4 90 80 80`). §2.3 cross-reference
  rephrased (`§4.1 trailing-octet tolerance` → `§4.1 v0.1.0 parser
  obligations`). §3.1 valid-predicate sentence completed with
  **MUST hold** main verb. §4.3 normative MUST against future
  amendment authors downgraded to non-normative SHOULD (v0.1.0
  implementations cannot enforce). §5.1 envelope strictness MUST
  added (verifier MUST require every `Required: yes` field). §5.1
  hex-decoding rules extended with explicit "no `0x`/`0X` prefix"
  non-example. §5.2 `result.path` row clarified — `""` ONLY for
  "no `0x2f` octet"; otherwise MUST begin with `/` (distinguishes
  the two zero-segment states). §7 normative MUST relocated into
  §5.1/§5.2 (§7 reduced to a process pointer to Story 1.7).
- Round-8 review JSON `_amendment_log` cleanup (2026-04-27): merged
  non-canonical ids `A-003-review-fold-in` and `A-005-round-7-fold-in`
  into canonical `A-003` / `A-005` entries (style-guide §2.1
  `^A-\d{3}$` schema). A-003 entry gains `deltas` array. A-005 entry
  drops the §4.1/B2 attribution from the `well-formed-host-with-tld-
  suffix` delta description (Round-7 P-1 rename retained the now-
  superseded forward-compat tolerance attribution). Renamed
  `well-formed-non-ascii-cyrillic` → `well-formed-idn-host-only` for
  symmetry with `well-formed-idn-host-and-path`. Extended
  `well-formed-host-and-utf8-path` with 3-byte and 4-byte UTF-8
  sequences (`/π汉💩` — U+03C0, U+6C49, U+1F4A9).
- Round-8 review vector-data fixes (2026-04-27, 3 BLOCKER patches):
  three UTF-8-rejection vectors had 16-byte filler hosts of
  `d2`/`d0`/`d1` octets that themselves invalidate UTF-8 at offset 17,
  defeating the named overlong/CESU-8 probes; replaced with `61` × 16
  (ASCII `'a'`) so parsers walk to the trailing target sequence.
  `well-formed-domain-510-host-and-2-path` expected `result.host` /
  `result.domain` corrected to match the wire bytes (510×`a` host,
  not 512×`a`). `malformed-domain-utf8-truncated-at-512-boundary`
  byte count corrected to `domain_len = 513` (510×`61` + `f0 9f 92`)
  to fire the rule-3-vs-rule-5 boundary as Round-7 P-13 prescribed.
- Round-7 review editorial improvements (2026-04-26, post-Round-6 closure
  code review): §1 precondition guard rewritten with single-threshold
  `total_len ≤ 16` form plus a `SIZE_MAX` clarifier. §3.1 valid-secret
  predicate scoped to MALFORMED rules 3/4/5 (rules 1 and 2 are buffer
  conditions, not field-level invariants). §3 producer constraints
  consolidated. §6.3 heading reframed as `RFC 1035 253-octet ceiling
  rejected — superseded by A-005 512-byte ceiling`. §6.3 prose: `path
  qualifiers` reverted to `port qualifiers` (Round-6 substitution typo).
  §5.4 `malformed-len-17` row reframed as zero-domain-octets boundary.
- Round-7 vectors (2026-04-26, 3 new entries, §5.4 inventory 22 → 25):
  `well-formed-domain-510-host-and-2-path` (A-005 boundary with path
  split — P-12), `malformed-domain-utf8-truncated-at-512-boundary`
  (A-005 + §2.1 rule 5 — P-13), `well-formed-host-and-utf8-path`
  (multi-byte UTF-8 path — P-14). `_amendment_log` gains an
  `A-005-round-7-fold-in` entry.
- A-005 (2026-04-26, Round-6 stakeholder decision Q1): Domain-field upper
  bound of 512 bytes. Partial walkback of A-004 length-silence following
  convergent DoS analysis (Sterling, Bob, Murat — Round 6). §2.1 gains
  MALFORMED rule 3: reject `domain_len > 512`. §6.3 updated to reflect
  512-byte normative ceiling (253-octet RFC 1035 cap still rejected). §6.8
  amended with A-005 walkback note. §1.1 "no upper bound" text replaced with
  A-005 ceiling statement. frontmatter `amendment_log` records A-005.

  ### Test Vectors (Story 7-1)
  Task 2 — 4 ceiling-boundary vectors in `secret-format:` array:
  `a005-domain-len-511` (511-byte host → PASS),
  `a005-domain-len-512-with-trailing-slash` (511-byte host + `/` = 512-byte domain → PASS, A-006 P-16 intersection),
  `a005-domain-len-513-reject` (513-byte domain → FAIL `MALFORMED` + `detail.rule="ceiling-exceeded"`),
  `a005-domain-len-1024-reject` (1024-byte domain → FAIL same, no DoS amplification).
  Task 10 — 2 additional byte-resolution boundary vectors:
  `a005-domain-len-0-reject` (0-byte domain → FAIL MALFORMED via rule 1),
  `a005-domain-len-1` (1-byte domain → PASS, minimum valid host).
  New lib detail-code: `T3_ERR_DOMAIN_TOO_LONG = -17` added to `lib/include/t3.h`
  (additive patch bump lib-v0.1.3). NOTE: wire-class remains `MALFORMED` per
  §2.1 rule 3; the lib code is surfaced via `expect.detail.lib_code` as a
  non-normative implementation hint.

- Round-6 review editorial improvements (2026-04-26): §2.1 restructured
  with a normative validation-order block (INVALID_ARG checks first, then
  MALFORMED checks in order) and explicit `INVALID_ARG rejection rules` /
  `MALFORMED rejection rules` subheadings. §2.3 UNSUPPORTED_VERSION clause
  narrowed per Q2: fires only for unrecognised markers; recognised future-
  version markers fall under §4.1 tolerance. §3.1 defines the "valid
  `t3_secret_t`" predicate. §1 gains a precondition guard on the
  `domain_len = total_len - 17` identity. §5.1 promotes "lowercase hex"
  to normative MUST; adds `args MUST contain exactly one string` rule;
  defines exit-2 conditions for uppercase hex, whitespace, and length>1
  arrays. §5.2 `result.key` MUST wording added. §5.4 `malformed-len-17`
  row reframed as length-below-minimum. §7 added: forward handoff to Story
  1.7 — v0.1.0 parsers MUST populate `result.host` AND `result.path`.
- Round-6 vectors (2026-04-26, 6 new entries, §5.4 inventory 16 → 22):
  `malformed-domain-overlong-utf8-c0af` (overlong `C0 AF`),
  `malformed-domain-overlong-utf8-e0-80-80` (overlong `E0 80 80`),
  `malformed-domain-cesu8-surrogate` (CESU-8 surrogate `ED A0 80`),
  `well-formed-domain-512` (512-byte domain, A-005 accept boundary),
  `malformed-domain-513` (513-byte domain, A-005 reject boundary),
  `forward-compat-recognised-marker-tolerance` (§4.1 v0.1.0 tolerance).
- Story 1-2 session-header parallel work (2026-04-26): conformance vectors
  for the wire-format session header land alongside Story 1-2's
  `spec/wire-format.md`. The `unit.json` `session-header` array is
  populated by Story 1-2; the `secret-format` array is owned by Story 1-1.
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

  ### Test Vectors (Story 7-1)
  Task 1 — 7 vectors in `secret-format:` array covering all A-003 boundary conditions:
  `a003-host-only` (host only, no `/`; `result.path=""`),
  `a003-host-and-path` (host + `/path`; `result.host` + `result.path` populated),
  `a003-trailing-slash` (`host + "/"` → `result.path="/"`),
  `a003-leading-slash-reject` (`/host` → MALFORMED),
  `a003-idn-host-only` (Punycode ASCII host; verbatim in `result.host`),
  `a003-idn-host-and-path` (Punycode host + path),
  `a003-double-slash-path` (`host//x`; path=`//x`, first-`/` split only).

- Initial scaffold: reading order, RFC 2119 discipline, banner conventions.

### Changed
- Round-7 P-1 (2026-04-26): renamed conformance vector
  `forward-compat-recognised-marker-tolerance` →
  `well-formed-host-with-tld-suffix`. The vector exercises a long ASCII
  host with a `.ext` suffix; v0.1.0 has no future-marker semantics, so
  the prior name implied tolerance behaviour the spec does not require.
- A-004 (2026-04-25, companion to A-003): Length-silence partially walked
  back by A-005 (2026-04-26 — 512-byte ceiling on `domain`). A-004's
  no-SHOULD stance still applies to `host_len` / `path_len` sub-fields
  (the 512-byte ceiling bounds them implicitly via the parent `domain`).
  §6.3 (RFC 1035 253-octet ceiling rejected) and §6.8 (length-prefixed
  path rejected) record the rejected alternatives.

### Removed
- Round-7 P-16 (2026-04-26): §2.1 normative clause "A parser MUST NOT
  return a different error class for the same input across conforming
  implementations" removed. v0.1.0 distinguishes only two error classes
  (`INVALID_ARG` vs `MALFORMED`); finer error sub-classification is
  deferred (see `deferred-work.md`).
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
