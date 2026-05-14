---
spec_version: 0.1.0-draft
last_updated: 2026-05-14
status: draft
---

> **Normative.** This document defines required behaviour for conforming
> Type3 implementations. Code in `lib/` is illustrative; where this
> document and `lib/` differ, this document wins.

# Anti-Probe Behaviour

Rules that make a Type3 server indistinguishable from a generic
HTTPS+WebSocket origin under DPI probing.

## 1. Silent close

The server MUST close misbehaving connections without emitting any
protocol-level error, banner, or close-code that distinguishes the
close from a generic TCP RST / FIN-by-timeout from an HTTPS+WebSocket
origin. The full normative rules — uniform-random delay, error-class
catalogue, idle-timeout asymmetry — live in §7.0. The adversary model
this defends is documented in [`threat-model.md`](threat-model.md)
"Defended properties".

## 2. Timing constraints

Silent-close delay is uniform-random in `[50 ms, 200 ms]` (§7.0). The
statistical-independence invariant that operationalises "no envelope
leaks parser internals" — TOST equivalence on length-bucket means +
bounded Spearman correlation — is normative in §8 (Timing Invariants).
See also [`epic-1-style-guide.md` §11](../../docs/epic-1-style-guide.md)
for the clock-source primitive (`CLOCK_MONOTONIC` only).

## 3. FR43 retry heuristic

The client-side retry schedule — tier-0 idle → tier-1 silent retry →
tier-2 backoff + degraded UI → tier-3 halt + explicit user surface,
with rolling-window triggers, NFR25 backoff sequence, and post-user-retry
state — is normative in §7.2. The FR19↔FR43 symmetry rule (client counts
every close uniformly regardless of cause) is documented at the end of
§7.2.

## 4. Logging

Server-local admin counters (e.g. `bad_header_drops`, `silent_close_count`)
MUST NOT leak information useful to a probe at default verbosity. Per
NFR19, server logs do not carry client IP at default verbosity. Per
NFR20, end-user-visible error strings never leak protocol-implementation
detail or fingerprintable identifiers. The eight silent-close error
classes catalogued in §7.0 MAY each increment a generic
`silent_close_total` counter but MUST NOT emit per-class identifiers
in the close path or default-verbosity log line; per-class breakdown
belongs in `--verbose` admin diagnostics and never on the wire.

## 7. Silent-close and FR43 retry-tier FSM _(normative)_

_Authority: `_bmad-output/planning-artifacts/epics.md` FR19 (server-side
50–200 ms silent close), FR43 (client-side tier-1/2/3 retry heuristic),
NFR25 (exponential backoff `1, 2, 4, 8, 16, 32, 60` s), AR-G3
(out-of-band key revocation only)._

### 7.1 Server-side silent close

#### 7.1.1 Uniform-random delay

On routing an offending connection to the silent-close path, the server
MUST sample a delay from the closed interval `[50 ms, 200 ms]` using a
uniform-random draw from the host RNG callback (the implementation-side
`rng` callback per `epic-1-style-guide.md` §10), and MUST hold the
connection open without writing any bytes until the sampled delay
elapses. After the delay, the server MUST close the connection at the
TCP layer (FIN, no RST preference) without emitting any
protocol-level error, banner, or close frame that distinguishes the
close from a generic HTTPS+WebSocket origin teardown.

The delay timer is anchored on the **parser-verdict timestamp** — the
moment the parser renders its verdict on the offending input. Anchoring
the timer before the verdict (e.g. on TCP accept, on first-byte arrival,
or on TLS handshake completion) is forbidden: a pre-verdict anchor
convolves TLS / parser wall-clock cost into the silent-close
distribution and leaks parser internals through the resulting bimodality.

#### 7.1.2 Error classes routed to silent close

The silent-close path applies to every connection terminated for any
of the eight error classes catalogued below. The same uniform
`[50 ms, 200 ms]` distribution applies to all eight — uniform error-class
anonymity IS the AR-C2 invariant operationalised at the spec level
(any per-class delay variance becomes a probe oracle by class).

| Error class | Trigger |
|-------------|---------|
| `malformed-prefix` | first byte does not match a valid `command_type` enumerant |
| `malformed-length` | length-prefix violates the wire-format size envelope |
| `short-input` | peer closed the connection (or the implementation otherwise detected premature transport close) before the parser had enough bytes for a verdict |
| `unsupported-version` | `version` byte outside the supported range; in-band negotiation already exhausted (see `wire-format.md` §6) |
| `nonzero-reserved-flags` | flags field carries bits the current spec reserves to zero |
| `replay-detected` | Session Header / nonce duplicates a value seen within the implementation's replay-protection window (window size is implementation-defined; not normative here) |
| `bad-mac` | obfuscated-2 layer integrity check fails (see `wire-format.md` §4) |
| `wire-format-fragmentation-exceeded` | WebSocket fragmentation policy budget exceeded |

For `short-input` specifically, the parser-verdict anchor is the moment
the implementation detects the premature transport close, not the moment
the connection was opened.

#### 7.1.3 Idle-timeout asymmetry (non-member)

The `idle-timeout` close is **NOT** a member of the silent-close set on
the server side: `idle-timeout` closes are immediate (no 50–200 ms
delay), because they are internal housekeeping, not adversarial
responses. Encoding the silent-close delay on idle-timeout would burn
server resources without raising the cost to a probe (the probe is gone
by definition; the timing channel doesn't add information). Server-
resource conservation outweighs delay-uniformity in that one case.

The client side, by contrast, MUST count `idle-timeout` closes
uniformly with adversarial closes (see §7.2 FR19↔FR43 symmetry) because
the client cannot reliably distinguish causes and must not develop a
heuristic that an adversary could spoof.

### 7.2 Key revocation (AR-G3)

There is **no in-band revocation channel**. A client probing with a
stale or revoked secret receives a silent close indistinguishable from
any other malformed-input close — adding any in-band revocation signal
would create a probe oracle ("does this key still exist?") and
contradict the silent-close uniformity invariant.

Recovery is operator-driven, out-of-band: secret rotation via the
installer / operator-tooling flow, optionally accompanied by the
Recovery Letter PDF artefact (see Story 1.5 — operator-artefact
contracts; spec landing in `spec/recovery-letter.md` is owed by
Story 1-5a). Client-side detection of a revoked secret is via FR43
tier-3 escalation: after the tier-3 threshold the client surfaces a
non-modal user prompt and the user re-acquires the secret out of band.

### 7.3 FR43 retry-tier FSM

#### 7.3.1 State table

| From state | Event | To state | Side effects |
|------------|-------|----------|--------------|
| tier-0 | first close-event | tier-1 | start sliding-window timer; record close timestamp |
| tier-1 | close-event, count < 3 within 60 s | tier-1 | append timestamp; evict events older than 60 s |
| tier-1 | close-event, count reaches 3 within 60 s | tier-2 | emit `degraded` UI signal (C1 indicator transitions to degraded variant); window retained |
| tier-1 | 60 s elapses with no close | tier-0 | clear window |
| tier-2 | close-event, count < 5 within 30 s (rolling sub-window inside the 60 s retention) | tier-2 | append timestamp; continue to evict per 60 s rule |
| tier-2 | close-event, count reaches 5 within 30 s | tier-3 | emit `halt` UI signal + non-modal user prompt |
| tier-2 | 60 s elapses with no close | tier-1 | de-escalate; window retained for the 60 s sliding rule |
| tier-2 (de-escalated) | additional 60 s elapses with no close (window now empty) | tier-0 | clear window |
| tier-3 | any close-event | tier-3 | sticky; no further escalation; emit no additional UI signal |
| tier-3 | `t3_retry_user_retry()` invoked | tier-1 | clear window; record this user action as a fresh first event |

Tier-precedence rule (review-finding P-6): when a single close-event
would simultaneously satisfy a tier-2 and a tier-3 trigger (e.g. the
5th close in 30 s also happens to be within a 60 s window where the
3rd already escalated to tier-2), tier-3 wins. The FSM does not visit
tier-2 transiently in that case.

#### 7.3.2 Retention and rolling-window semantics

There is a single retention window of 60 s. Events older than 60 s are
evicted on every close-event. The 30 s threshold for the tier-2 → tier-3
escalation is a sub-window *inside* the 60 s retention — implemented
as "count events whose timestamps fall in the most recent 30 s of the
retained window."

#### 7.3.3 Post-user-retry state

After the user explicitly retries from tier-3 via the
`t3_retry_user_retry()` action, the FSM enters **tier-1** with a cleared
sliding window; the next close-event starts a fresh tier-1 evaluation
(NOT tier-0 idle, NOT a probationary tier-2). Entering tier-1 (not
tier-0) is deliberate: a tier-0 entry would grant the user a free
"first close" that could mask an immediate re-failure; tier-1 entry
preserves the contract that any close from the current endpoint counts
toward the next escalation gate.

User-retry actions are themselves rate-limited (review-finding P-7):
two consecutive `t3_retry_user_retry()` invocations from the same
session MUST be separated by ≥ 5 s of wall-clock; invocations within
the cooldown are silently ignored by the lib and do NOT clear the
window. The 5 s floor matches the minimum jitter envelope of the first
retry tier and prevents an automated `t3_retry_user_retry()` loop from
acting as an in-band probe of the silent-close path.

#### 7.3.4 Backoff and jitter

A single jitter model applies to ALL three tiers: the NFR25 backoff
base sequence `1, 2, 4, 8, 16, 32, 60` seconds (attempts 1..7+),
multiplied per attempt by uniform `[0.8, 1.2]` jitter sampled from the
host RNG callback. For attempt numbers beyond the sequence length
(attempt ≥ 8), the backoff is capped at 60 s × jitter (review-finding
P-8); the cap holds indefinitely until the next tier transition or
user-retry resets the count.

Tier-specific behaviour changes only the **UI signal** emitted on
entering the tier; the underlying backoff math is shared. The backoff
delay and the 60 s retention window are orthogonal — the backoff
governs the *delay before the next connection attempt*, while the
retention window governs *tier-up / tier-down trigger counting* over
the most recent 60 s of observed close events.

#### 7.3.5 FR19 ↔ FR43 symmetry

The server's silent-close timing (FR19) and the client's retry-tier
escalation (FR43) are designed as complementary halves of the same
anti-probe contract. The client interprets ANY close from the current
endpoint as a tier event, regardless of underlying cause; idle-timeout
closes that the server emits *without* the 50–200 ms delay still count
toward client-side tier windows because the client cannot — and must
not need to — distinguish causes. The server's idle-timeout asymmetry
(no delay) and the client's blind counting (every close counts) are
intentional and meet at this seam.

## 8. Timing Invariants _(normative)_

_Authority: epic-1-style-guide.md §12 (Statistical rigour for AR-C2).
Historical referent: `_bmad-output/planning-artifacts/epics.md` AR-C2 bullet._

### 8.1 Invariant statement

Silent-close delay distribution is statistically independent of input
content and input length. Independence is operationalised as the
conjunction of:

(a) **TOST equivalence on length-bucket means** (§8.2): for every pair
of length buckets (i, j), the absolute difference between bucket means
is bounded below the equivalence margin δ = 2 ms; and

(b) **Bounded Spearman correlation** (§8.2): |ρ| < 0.1 over the pair
`(input_len, close_delay_ns)`.

Both conditions MUST hold for the invariant to pass. Either condition
failing constitutes an AR-C2 violation.

### 8.2 Test method

#### Length buckets

The canonical bucket edges are pinned verbatim below and MUST NOT be
redefined by downstream implementations. They are reproducibility-locked
across forks:

```yaml
bucket_edges:
  - [0, 63]
  - [64, 255]
  - [256, 1023]
  - [1024, 4095]
  - [4096, 16383]
```

5 buckets yield C(5,2) = 10 pairwise comparisons. Bucket assignment is
closed-interval (`lo ≤ input_len ≤ hi`). Inputs above 16 383 bytes are
outside AR-C2 scope until a future spec amendment widens the range; if
such inputs are nonetheless produced (e.g. by a non-conforming harness),
gate implementations MAY clamp them into the top bucket `[4096, 16383]`
for processing parity with `lib/fuzz/analyse.py` rather than raise. The
clamp is a defense-in-depth alignment, not a normative widening of scope.

#### TOST parameters

| Parameter | Value |
|-----------|-------|
| Equivalence margin δ | 2 ms = 2 000 000 ns |
| Family-wise α | 0.05 |
| Per-pair α (Bonferroni) | 0.05 / 10 = **0.005** |
| Number of pairwise tests | 10 (C(5,2)) |

Each of the 10 bucket-pair tests is a two one-sided test (TOST) on the
bucket means at α_per_pair = 0.005. The run passes ONLY when every pair
rejects both non-equivalence nulls.

#### Spearman ρ assertion

Spearman ρ is computed over the pair `(input_len, close_delay_ns)` ONLY
(not `input_hash` — hash has no metric ordering). The assertion |ρ| < 0.1
is applied ONLY when total N ≥ N_required (per §8.3). Below the gate the
run is underpowered and the Spearman assertion is non-gating; the run
MUST emit `WARN: underpowered` in that case.

#### Metric definition

`close_delay_ns` is the **parser-verdict-anchored monotonic-ns delta**
emitted by the lib-internal timing engine (`t3_silent_close_delay_sample_ns`
via `CLOCK_MONOTONIC`). Wall-clock or accept-time-anchored deltas are NOT
accepted. See epic-1-style-guide.md §11 (clock source) and Story 1-7
(timing engine implementation).

### 8.3 Sample-size gate

The per-bucket sample-size gate N_required is derived from a pilot σ̂
measurement on the reference dispatcher. The normative pilot result is
committed at:

```
teleproto3/conformance/baselines/lib-v0.1.x/ar-c2-pilot.yaml
```

The derivation formula is:

```
n_per_bucket = ceil( 2 × (z_{1-α} + z_{1-β/2})² × σ² / δ² )
```

With:
- α = 0.005 (Bonferroni-adjusted per §8.2)
- β = 0.20 (80% statistical power)
- z_{1-α} = z_{0.995} ≈ 2.576
- z_{1-β/2} = z_{0.90} ≈ 1.282
- σ = max(σ̂\_bucket\_i) across populated buckets (worst-case bucket drives the gate)
- δ = 2 000 000 ns

The pilot file's `n_per_bucket_required` field is the **spec-side gate**.
If the computed value is below 10 000, the **effective gate is
max(n_per_bucket_required, 10 000)** — do not gate below 10 000 on
principle (the 10 000 floor is the style-guide §12 minimum; the pilot
formula may yield a smaller number for fast parsers whose σ << δ).

A re-pilot is required whenever the lib timing engine undergoes a
material edit (i.e. changes to `lib/src/timing.c` — see style-guide
§14.1). Re-pilot is NOT required on every PR; cosmetic or unrelated
changes do not trigger it.

Runs with per-bucket sample count below the effective gate MUST emit
`WARN: underpowered` and are non-gating.

### 8.4 Implementation cross-reference

The two normative enforcement points for this invariant are:

- **`teleproto3/lib/fuzz/analyse.py`** (Story 1-10, fuzz path): the
  post-processor for side-channel timing logs; computes TOST + Spearman
  and gates on the thresholds above. Bucket edges already aligned with
  §8.2 canonical edges.

- **`teleproto3/conformance/gates/timing_invariant.py`** (Story 7-2,
  CI-gate path): the conformance-suite gate that runs the timing invariant
  assertion in CI. Story 7-2's pre-1-3a implementation used Mann-Whitney U
  with a 10-bucket variant — this is method drift that Story 7-2 fixes per
  its D1 resolution (Mann-Whitney → TOST + Bonferroni, 10 buckets → 5
  buckets per §8.2). This story authors the contract, not the rewrite.

No implementation prose for either enforcement point lives in this spec
section; the above are cross-references only.

> Mann-Whitney U is explicitly REJECTED as the AR-C2 method. See §11
> Contested Decisions and style-guide §14.8.

## 9. AR-C4 per-release baseline methodology _(normative)_

_Authority: `_bmad-output/planning-artifacts/epics.md` AR-C4 (per-release
baseline + ±15% drift gate); `epic-1-style-guide.md` §11 (clock source)
and §12 (statistical rigour)._

### 9.1 Baseline artefact

Every conforming release of `libteleproto3` MUST commit a per-version
baseline file at:

```
teleproto3/conformance/baselines/<lib-version>.yaml
```

The file records the silent-close delay distribution and timing-invariant
statistics measured against the release's reference dispatcher.
`<lib-version>` matches the `T3_LIB_VERSION_*` macros (e.g.
`lib-v0.1.0.yaml`). The schema is normative.

### 9.2 Schema

```yaml
spec_version:        "0.1.0-draft"
lib_version:         "lib-v0.1.0"
captured_at:         "YYYY-MM-DDTHH:MM:SSZ"
clock_source:        "CLOCK_MONOTONIC"           # style-guide §11; never _RAW / REALTIME / gettimeofday / rdtsc
sample_count:        100000                      # total across all buckets
buckets:             [[0,63],[64,255],[256,1023],[1024,4095],[4096,16383]]
p50_delay_ms:        125.0
p95_delay_ms:        192.0
p99_delay_ms:        198.5
tost_delta_ms:       2.0
tost_n_per_bucket:   10000                       # effective gate per §8.3 (max of pilot σ̂ derivation and 10 000 floor)
tost_pass:           true                        # all 10 bucket-pairs reject non-equivalence at α_per_pair = 0.005
spearman_n:          50000
spearman_rho:        -0.012
spearman_ci95_low:   -0.020
spearman_ci95_high:  -0.004
drift_gate:
  p50_pct:           "+0.0%"                     # vs previous release; ±15% per-percentile
  p95_pct:           "+1.2%"
  p99_pct:           "+0.4%"
  rho_pct:           "-3.0%"                     # see §9.4 floor rule for near-zero ρ
rationale:           ""                          # required only if any |drift| > 15% OR an absolute-floor breach (§9.4)
```

The bucket edges in the YAML MUST match the §8.2 canonical edges
verbatim — reproducibility across releases survives spec-version skew
only when every release records the bucket-partition explicitly.

### 9.3 Capture procedure

Per-release baseline capture (the `make baseline` target — implementation
in Story 1.7):

1. Spin up the lib timing engine in isolation (no server, no network
   adapter, host RNG fixed-seed for the run).
2. For each of the five §8.2 buckets, generate ≥ `tost_n_per_bucket`
   synthetic inputs of the bucket's length range; for each input,
   record `(input_len, close_delay_ns)` returned by the timing engine
   driven through the parser-verdict path.
3. Aggregate into per-bucket vectors; compute p50 / p95 / p99 across
   the union of all buckets.
4. Run TOST on each pair of buckets with the §8.2 parameters (δ = 2 ms,
   α_per_pair = 0.005); record `tost_pass: true` only when every pair
   rejects both non-equivalence nulls.
5. Compute Spearman ρ on the joined `(input_len, close_delay_ns)`
   vector with N ≥ `tost_n_per_bucket` (gate); record ρ and the
   bootstrap 95% CI.
6. Apply the §9.4 drift gate to populate `drift_gate.*` and the
   `rationale` field.

### 9.4 Drift gate

Each of `p50_delay_ms`, `p95_delay_ms`, `p99_delay_ms`, `spearman_rho`
is checked **independently** against the previous release. If ANY one
of the four drifts by more than ±15% (relative), a minor-version bump
is REQUIRED and the `rationale:` field MUST be populated with a
human-readable explanation of the drift cause.

**Initial-release exception (review-finding P-9).** The first baseline
in a fresh `lib-v0.M.x` minor-version line has no predecessor: the
drift gate is SKIPPED for that file. `drift_gate.*` MAY be omitted in
the initial baseline, or set to `"n/a (initial baseline)"`. The next
release in the same minor-version line is the first to apply the gate.

**Near-zero Spearman floor (review-finding P-3).** Relative percent
drift on `spearman_rho` is mathematically ill-conditioned when the
previous value is near zero (a typical healthy result is |ρ| < 0.02
under measurement noise). When |ρ_prev| < 0.02, the relative gate is
SUPPLEMENTED by an absolute-floor rule: a drift |ρ_new − ρ_prev| > 0.05
in absolute terms triggers the same rationale + bump requirement,
regardless of percent calculation. This prevents a regression from
ρ = 0.001 to ρ = 0.08 from masquerading as "within ±15%" (an absurd
1 480 000% relative drift that the gate would either flag spuriously
or silently overflow).

**Absolute vs relative precedence (decision DN-5).** When both gates
apply (i.e. |ρ_prev| < 0.02 AND the relative gate also fires), EITHER
condition triggers the bump-and-rationale obligation. The two gates
are an OR, not an AND.

### 9.5 Re-pilot trigger

The per-bucket sample-size gate `tost_n_per_bucket` is derived per §8.3
from a pilot σ̂ measurement on the reference dispatcher. A re-pilot is
REQUIRED whenever the lib timing engine undergoes a material edit
(changes to the timing-engine TU under `lib/src/`, currently
`t3_timing.c`). Cosmetic edits (comments, formatting, unrelated test
fixtures) do not trigger a re-pilot. The judgement of "material"
belongs to the lib maintainer; the re-pilot artefact at
`conformance/baselines/<lib-version>/ar-c2-pilot.yaml` is the evidence
of compliance.

## 10. AR-C8 fuzz harness contract _(normative — contract only)_

_Authority: `_bmad-output/planning-artifacts/epics.md` AR-C8 (fuzz
harness); `epic-1-style-guide.md` §12. Implementation: Story 1-10
`done` — see `teleproto3/lib/fuzz/`._

The AR-C8 fuzz harness MUST log `(input_len, close_delay_ns)` per
input, partition into the §8.2 canonical buckets, and gate on TOST +
Spearman per §8.2 / §8.3. The implementation is the artefact under
`teleproto3/lib/fuzz/` (Story 1-10): the libFuzzer driver, the custom
mutator producing length-bucketed corpora, and the post-processor
`teleproto3/lib/fuzz/analyse.py` that computes the §8 gates.

This section deliberately carries **no implementation prose** — the
named source files are normative. Adding implementation detail here
would duplicate scaffold reality and invite drift (§14 anti-pattern
#3). The conformance obligation is: a Type3 reference dispatcher MUST
ship a fuzz harness with these properties and pass the §8 gates against
its own baseline.

## 11. Contested Decisions _(normative — audit trail)_

_Authority: AR-C9 (Contested Decisions discipline)._

This section records alternatives that were rejected during the design
of §§7–9. A reviewer revisiting any of these proposals MUST cite this
section before re-litigating; an absent rationale here is grounds to
reopen, a present rationale is grounds to defer-without-reopening.

### 11.1 Rejected: deterministic silent-close delay

**Alternative.** A fixed delay (e.g. exactly 125 ms) instead of the
uniform `[50 ms, 200 ms]` distribution of §7.1.1.

**Reason for rejection.** A fixed delay is trivially fingerprintable —
the entire purpose of the 50–200 ms window is statistical opacity to
the adversary's timing models. A point-mass distribution gives the
adversary a perfect oracle to confirm "this endpoint is a Type3 server"
after a single probe. Uniform randomness over a 150 ms window costs
the legitimate-client UX nothing measurable and forces the adversary
to absorb the full distribution variance.

### 11.2 Rejected: in-band key-revocation channel

**Alternative.** Add a protocol-level signal that distinguishes "stale
key, please rotate" from generic malformed-input silent close.

**Reason for rejection.** Any in-band revocation signal becomes a probe
oracle: an adversary can submit known keys and observe whether they
elicit the revocation signal vs the generic silent close, learning
the key-rotation state of every operator. This contradicts the
silent-close uniformity invariant. The only sound recovery path is
out-of-band re-issuance (operator-driven secret rotation), tied to the
Recovery Letter PDF artefact in Story 1.5 / Story 1-5a. AR-G3 codifies
this constraint.

### 11.3 Rejected: longer silent-close upper bound (200+ ms)

**Alternative.** Extend the §7.1.1 uniform-random window beyond 200 ms
(e.g. `[50 ms, 500 ms]`) on the theory that more variance frustrates
adversary timing models harder.

**Reason for rejection.** The legitimate-client startup-UX cost scales
linearly with the upper bound while the marginal probe-deterrence
flattens out: beyond ~200 ms, network RTT noise (which the adversary
already absorbs) dominates the timing-channel variance the silent-close
delay introduces. The 200 ms upper bound is the inflection point at
which additional variance buys nothing the adversary can't already
attribute to natural network jitter. Set the bound where it bites the
adversary, not where it bites the user.

### 11.4 Rejected: exponential delay distribution

**Alternative.** Replace the uniform `[50 ms, 200 ms]` distribution
with an exponential or other heavy-tailed distribution on the theory
that heavier tails hide the bucket structure better.

**Reason for rejection.** A heavy tail leaks bucket structure under
aggregate timing analysis: the tail samples are over-represented in
any given length-bucket, which gives the adversary correlation signal
between input length and tail-sample count. This directly contradicts
AR-C2's Spearman bound. Uniform is the structurally correct choice
because it equalises sample density across the window per length-bucket
and gives the adversary nothing to correlate against.

### 11.5 Rejected: Mann-Whitney U as the AR-C2 method

**Alternative.** Test silent-close-distribution independence with
Mann-Whitney U on length-bucket pairs, gating on `p > 0.05`.

**Reason for rejection.** "Failing to reject the null at p > 0.05" is
not evidence of equivalence; it is the absence of evidence. A small
sample with high variance trivially fails to reject and would be
misread as a pass. The correct test for equivalence is TOST (two
one-sided tests), which the §8 invariant adopts. This rejection is
codified in `epic-1-style-guide.md` §14.8 (anti-pattern catalogue) and
ERRATA `E-002` (Story 1-3a). The historical AR-C2 phrasing in
`_bmad-output/planning-artifacts/epics.md` retains the Mann-Whitney
wording for audit-trail continuity, with an additive supersession
marker per Story 1-3a AC #8.
