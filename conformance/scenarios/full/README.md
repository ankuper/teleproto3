# Full scenarios — Full compliance

Full compliance = Mandatory + this directory. An IUT claiming **Full**
MUST pass every scenario here on top of `../mandatory/`.

## Scope

_TBD(conformance-v0.1.0):_ seed list.

- Rotation (client-initiated and server-command-slot-initiated).
- §6 version negotiation traces.
- UX state machine conformance — if the IUT exposes a UI, the runner
  drives it through `../../vectors/rotation-scenarios.yaml` and
  validates the state transitions against
  `../../spec/ux-conformance.md §3`.
- Full `ux-strings.yaml` key coverage (en/ru/fa/zh).
- Fragmentation vectors from `../../vectors/fragmentation.yaml`.

An IUT without a UI (e.g. a headless library integration test) MAY
skip the UX state-machine scenarios and still claim Full, but MUST
declare that limitation in its conformance report.
