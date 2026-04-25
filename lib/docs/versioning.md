# lib ↔ spec compatibility

libteleproto3 uses independent semver from `spec/`. Each `lib-vX.Y.Z`
release declares the `spec-vA.B.C` range it implements via the
`implements_spec` field in `lib/VERSION`.

## Rules

- `lib` PATCH and MINOR bumps MUST NOT change the `implements_spec`
  range. Only `lib` MAJOR may.
- A `lib` release that implements an unreleased `spec-vX.Y.Z` MUST NOT
  be published until that spec tag is also published.
- Consumers pin to a specific `lib-vX.Y.Z` tag (not a range). The
  consumer's CI validates the declared `implements_spec` against the
  spec version the consumer expects.

## Cross-version CI gate (Amelia's rule)

A `lib-vX.Y.Z` tag is **not considered "final"** until all three
client forks (tdesktop, iOS, Android) have reported CI-green against
the preceding RC tag (`lib-vX.Y.Z-rc.N`). The release pipeline:

1. Maintainer pushes `lib-vX.Y.Z-rc.1`.
2. `consumer-dispatch.yml` fires; each client fork's bump-bot opens a PR
   pinning the RC.
3. Each fork's CI runs against the RC and reports status back to the
   monorepo (repository_dispatch or a small sidecar service).
4. Only when all three CIs are green does the maintainer move the tag
   to `lib-vX.Y.Z` (final).

Without this gate, client forks drift into silent incompatibility with
each other — the automation exists because splitting libteleproto3 into
its own repo later becomes infeasible if pinning discipline was loose.

## Bump discipline

- PATCH: fixes that don't change the public header nor observable
  wire behaviour.
- MINOR: additive changes to the public header (new functions/fields);
  spec range MAY widen.
- MAJOR: breaking changes to the public header OR to wire behaviour
  that conforms to a newer spec MAJOR.
