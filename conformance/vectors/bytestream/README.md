# bytestream vectors

Captured bytestreams for replay scenarios. Each scenario is a pair:

- `<scenario-id>.tdesktop.bin` — raw bytes captured from a known-good
  reference session (currently, tdesktop; other sources MAY be added).
- `<scenario-id>.meta.yaml` — metadata: capture date, source
  implementation + version, the test property the stream establishes,
  and the spec sections it exercises.

## Why capture from tdesktop

tdesktop is the internal reference client (Cat 3). Its behaviour is
treated as a concrete interoperability baseline while the spec is
drafted. Captured streams are **not** normative; the spec is. But they
let the harness verify that other IUTs produce byte-equivalent output
under identical inputs.

## Naming

- `scenario-id` matches a scenario manifest id under `../../scenarios/`.
- Captures are pinned to specific tdesktop commit SHAs; update `meta.yaml`
  accordingly.

## Rotating a capture

When tdesktop semantics change such that a capture becomes stale:
1. Re-capture with the new commit.
2. Update `meta.yaml`: `source_sha`, `captured_at`.
3. Bump the `conformance-vX.Y.Z` tag.
4. `vectors-detect-drift.yml` notifies consumer repos.

## Scaffold state

_Empty at conformance-v0.1.0-draft._ First captures land during the
spec-v0.1.0 drafting epic.
