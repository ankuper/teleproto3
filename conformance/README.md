# Conformance harness

Language-agnostic harness for verifying any implementation of the
Type3 protocol against the normative `spec/`.

## Why language-agnostic

The harness drives an implementation-under-test (IUT) as a subprocess
over a stdin/stdout JSON protocol (see [`protocol.md`](protocol.md)).
That means a Rust, Go, Python, Dart, Swift, or Kotlin implementation
can be verified with the same vectors as the C reference
implementation in `lib/`. No language coupling.

Normative contract: [`../spec/conformance-procedure.md`](../spec/conformance-procedure.md).
This directory is the executable counterpart.

## Layout

- [`protocol.md`](protocol.md) — stdin/stdout JSON contract the IUT
  speaks.
- [`runner/run.sh`](runner/run.sh) — POSIX runner. Invokes an IUT,
  executes a scenario set, writes a PASS/FAIL report.
- [`runner/verify.py`](runner/verify.py) — verifies IUT output against
  expected vectors.
- [`vectors/`](vectors/) — test vectors, pinned to a `spec-vX.Y.Z`
  + `lib-vX.Y.Z` pair.
  - `unit.json` — per-op unit vectors (schema v1.0).
  - `fragmentation.yaml` — frame-fragmentation edge cases.
  - `rotation-scenarios.yaml` — secret/domain rotation traces.
  - `bytestream/` — captured bytestreams + meta for replay scenarios.
- [`scenarios/`](scenarios/) — executable scenario manifests per
  compliance level:
  - [`mandatory/`](scenarios/mandatory/) — Core compliance.
  - [`full/`](scenarios/full/) — Full compliance.
  - [`extended/`](scenarios/extended/) — future / optional.

## Running

```sh
# Against the in-repo reference implementation:
./runner/run.sh --impl ../lib/examples/c-client-side/client_example

# Against an external IUT:
./runner/run.sh --impl /path/to/your/iut --level core
```

Output: one line per scenario (`<id>  PASS|FAIL  <notes>`) followed
by a summary. Exit code is zero iff all scenarios in the selected
level passed.

## Versioning

Tagged `conformance-vX.Y.Z` independently. Each release pins a spec
range via `validates_spec` in `VERSION`. External implementations
should cite both tags in their conformance reports — see
[`../.github/ISSUE_TEMPLATE/conformance-report.md`](../.github/ISSUE_TEMPLATE/conformance-report.md).

## Licence

Apache 2.0 — see [`LICENSE`](LICENSE).
