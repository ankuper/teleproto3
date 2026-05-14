---
spec_version: 0.1.0-draft
last_updated: 2026-04-27
status: draft
---

> **Normative.** This document defines required behaviour for conforming
> Type3 implementations. Code in `lib/` is illustrative; where this
> document and `lib/` differ, this document wins.

# Errata

Append-only log of defects, ambiguities, and clarifications filed against
published spec tags. New entries fold into the next MINOR release.

## Process

1. File a [`spec errata`](../.github/ISSUE_TEMPLATE/errata.md) issue.
2. Maintainer acknowledges within 30 days (see [OWNERSHIP.md](../OWNERSHIP.md)).
3. If accepted, an entry is appended below with an ID (`E-NNN`), the
   affected section, the ambiguity, and the clarified reading.
4. The clarification is rolled into the next MINOR spec release; the
   errata entry remains here for historical record.

## Entries

### E-001 — KDF derivation rationale (spec/wire-format.md §4)

- **Filed:** 2026-04-27 (Round-8 close-out, Story 1-2)
- **Affects:** `spec/wire-format.md` §4.1 / §4.2 (obfuscated stream — AES-256-CTR setup)
- **Status:** rationale-only entry; no normative change. The §4.1 / §4.2
  text in wire-format.md is the binding contract.

**Rationale.** §4 lifts the KDF from ERRATA-deferred prose to normative
inline formulae. The slice indices (`random_header[8..40]`,
`[40..56]`, `[24..56]`, `[8..24]` and the role of `secret[0..16]`) are
not free design choices — they reproduce the v1 MTProto-obfuscated2
KDF used by every existing Telegram client and MTProxy deployment.
Type3 inherits the construction byte-for-byte to preserve interop with
existing obfuscated2 clients on the wire.

**Source-of-truth citation.** The canonical reference implementation
is `mtproxy3-legacy/src/net-tcp-rpc-ext-server.c:1407–1467` — the
`tcp_rpcs_compact_parse_execute` initial-handshake block. The slice
indices, the `reverse(...)` helper, the SHA-256 keying, the
`(read_key, read_iv, write_key, write_iv)` quadruple naming, and the
`{0xdddddddd, 0xeeeeeeee, 0xefefefef}` magic-tag admission set all
trace to that block. Reviewers verifying §4.2 against the v1 KDF
should diff against those line numbers; any future divergence is a
breaking change requiring a new amendment entry on `wire-format.md`.

**Per-session uniqueness.** `random_header` is 64 bytes from a CSPRNG
on the client side; a fresh `random_header` per session yields a fresh
`(read_key, read_iv, write_key, write_iv)` quadruple. AES-CTR counter
reuse across sessions sharing the same secret is therefore
structurally impossible, provided §4.1 invariant 1 (no derivation
inputs other than `random_header`) holds. The 16-byte `secret`
protects authenticity (gating admission via the magic tag), not
confidentiality of `random_header`; `random_header` is wire-visible
by construction.

**Why now.** Round-6 review (Sterling #1, Bob #3) flagged the §4
deferral as too soft for a v0.1.0 close-out: an implementer of Story
1.7 (`libteleproto3`) cannot ship without a binding KDF contract,
and "deferred to errata" left the four MUSTs (no IP/timestamp
derivation, continuous CTR, secret zeroisation, per-session
freshness) unstated. Round-8 party-mode resolved D1 by lifting
formulae inline (§4.2 normative) rather than splitting derivation
across spec + ERRATA, with this entry preserving the audit trail to
the v1 reference and the design rationale that informed the choice.

## E-002 — TOST supersedes Mann-Whitney for AR-C2

- **Filed:** 2026-05-13 (Story 1-3a — §8 normative landing)
- **Affects:** `spec/anti-probe.md` §8 + `_bmad-output/planning-artifacts/epics.md` AR-C2 bullet
- **Status:** normative supersession

**Scope.** The AR-C2 timing invariant was previously described in
`epics.md` as "Mann-Whitney U over length-bucketed samples, p > 0.05".
This phrasing is a style-guide §14.8 anti-pattern: using `p > 0.05
fails to reject null` as evidence of independence. The absence of
evidence is not evidence of absence — Mann-Whitney U cannot establish
equivalence.

**Supersession.** The normative AR-C2 test method is now defined in
`spec/anti-probe.md` §8 (this story) as:

- **TOST** (two one-sided tests) on length-bucket means with δ = 2 ms,
  family-wise α = 0.05, Bonferroni-adjusted α_per_pair = 0.005 for the
  10 pairwise tests across 5 canonical buckets ([0,63]/[64,255]/
  [256,1023]/[1024,4095]/[4096,16383]).
- **Spearman ρ** threshold: |ρ| < 0.1 over `(input_len, close_delay_ns)`,
  asserted only when total N ≥ N_required.
- **N_required** derived from pilot σ̂ via the TOST sample-size formula;
  committed at `conformance/baselines/lib-v0.1.x/ar-c2-pilot.yaml`; safety
  floor max(formula_result, 10 000).

**Anti-pattern citation.** Style-guide §14.8 explicitly bans
`p > 0.05 fails to reject null` as an evidence claim; use TOST (§12).
Mann-Whitney U is explicitly rejected in `spec/anti-probe.md` §8.4
forward reference to §11 Contested Decisions.

**`epics.md` marker.** The AR-C2 bullet in `epics.md` carries an additive
supersession marker per Story 1-3a AC #8; original phrasing is retained
for audit-trail continuity.
