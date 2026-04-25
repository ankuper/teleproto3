# Release cadence — server/

## Versioning

- Tag prefix: `server-vX.Y.Z`.
- Each release records the `lib-vX.Y.Z` it links against via the
  `linked_lib` field in `server/VERSION`.
- Server PATCH and MINOR bumps MUST NOT require a `lib` MAJOR bump.

## Cadence

_TBD(server-v0.1.0)_: finalise post-v0.1.0. Draft policy:

- **PATCH**: as needed for security + ops-critical fixes. No fixed
  cadence; time-to-publish is an SLA (see OWNERSHIP.md).
- **MINOR**: approximately quarterly, coupling fork-local additions
  and absorbed upstream subtree pulls.
- **MAJOR**: only on a `lib` MAJOR that requires a server-side
  breaking change, OR on a deliberate operational break (e.g.
  retiring a config flag).

## Coupling with upstream MTProxy

Upstream drives subtree pulls; pulls do not force a server-release.
A pull that only touches `common/`, `crypto/`, `jobs/`, `mtproto/`
may ship as a PATCH if it doesn't change observable fork behaviour.

## Release checklist

_TBD:_ finalise. Seed:

- [ ] `CHANGELOG.md` updated and dated.
- [ ] `VERSION` bumped; `linked_lib` reflects the lib release.
- [ ] `server/tests/dispatch/` + `server/tests/integration/` green.
- [ ] `conformance/` harness green against the reference IUT.
- [ ] `server/UPSTREAM.md` updated if this release folded in a subtree pull.
- [ ] GHCR image tagged + signed.
- [ ] Runbooks reviewed for accuracy against config changes in this release.
