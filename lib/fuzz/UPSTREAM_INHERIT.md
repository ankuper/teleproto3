# lib/fuzz/UPSTREAM_INHERIT.md — AR-S15 Upstream Inheritance Record

**Source commit:** `teleproxy/teleproxy@0cadcff`  
**Architecture reference:** `architecture.md §Category 15` — row `0cadcff libFuzzer on obfuscated2 → AR-C8 inherit + extend`  
**Conflict surface path:** `server/UPSTREAM.md` (per architecture.md §Category 15)  
**Story:** 1-10 (fuzz harness with input-independence assertion)

---

## Inheritance Table

| Concern | Source | Decision |
|---------|--------|----------|
| libFuzzer entry-point boilerplate (`LLVMFuzzerTestOneInput`, `LLVMFuzzerInitialize`) | Inherited verbatim from `0cadcff` | Used as structural scaffold; adapted for Type3 session API |
| Corpus seed for obfuscated2 frame parser | Inherited verbatim from `0cadcff` | N/A — obfuscated2 corpus not used in this story's harnesses; Type3 parsers use fresh corpora |
| Build rules for fuzzer-only library link (`-fsanitize=fuzzer` separate from `-fsanitize=address,undefined`) | Partial inherit from `0cadcff` | Adapted for `libteleproto3.a` and the two-mode (crash/timing) split |
| Session Header parser fuzz (`t3_header_parse`) | Not in upstream | **Authored fresh** — Type3 wire format; no upstream analogue |
| Secret-format parser fuzz (`t3_secret_parse`) | Not in upstream | **Authored fresh** — Type3 secret format; no upstream analogue |
| Timing-side-channel emission (`parse_ns`, `total_ns` schema) | Not in upstream | **Authored fresh** — AC#4 log schema; upstream has no timing emission |
| Spearman + Kendall + TOST `analyse.py` | Not in upstream | **Authored fresh** — `lib/fuzz/analyse.py`; stdlib-only, no numpy/scipy |
| `parse_ns` vs `total_ns` dual-field schema | Not in upstream | **Authored fresh** — AC#4 design decision (see Dev Notes §"Side-channel log schema") |
| Byte-order round-trip seed corpus (`corpus/header_byteorder/`) | Not in upstream | **Authored fresh** — routing from 1-6 code review (LE flags regression guard) |
| `LLVMFuzzerCustomMutator` for length-bucket coverage | Not in upstream | **Authored fresh** — secret-format bucket-edge bias (optional; NOT the timing hook) |

---

## Notes for Future Upstream Pulls

1. **Conflict surface**: any future `git subtree pull` of `teleproxy/teleproxy` may update `server/UPSTREAM.md`, not files under `lib/fuzz/`. The `lib/fuzz/` directory is fork-local; it does NOT live inside the `server/` subtree path.

2. **Obfuscated2 corpus delta**: if upstream adds more corpus seeds for the obfuscated2 harness, those are isolated to the `server/` subtree and do not automatically flow to `lib/fuzz/`. A deliberate copy step is needed, documented in `server/UPSTREAM.md`.

3. **Timing emission anti-pattern guard**: if upstream introduces timing code inside a `LLVMFuzzerCustomMutator`, that is an upstream bug — `CustomMutator` runs before execution and never has access to real parse latency. This harness explicitly guards against this (`anti-pattern: if sc_emit is found inside CustomMutator body, fail review` — Dev Notes §Anti-pattern guards).
