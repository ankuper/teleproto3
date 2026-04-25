# Contributing to teleproto3

Thanks for your interest. A few ground rules keep this repo coherent.

## What lives where

- `spec/` — normative protocol specification. Changes here change the
  protocol. Use RFC 2119 keywords (MUST, SHOULD, MAY) in their UPPERCASE
  normative sense only.
- `lib/` — reference C implementation (`libteleproto3`). Must track spec.
  When they differ, spec wins.
- `server/` — fork of MTProxy, vendored via `git subtree`. Avoid editing
  upstream-managed subdirectories (`common/`, `crypto/`, `jobs/`, `mtproto/`)
  outside of a subtree pull.
- `conformance/` — language-agnostic harness + test vectors. Scenarios are
  pinned with `spec/` + `lib/` tags.

## Banner discipline

Every `lib/src/*.c` file begins with the reference-implementation banner.
Every `spec/*.md` file begins with the normative frontmatter + banner.
CI (`.github/workflows/banner-discipline.yml`) enforces this. See the
[architecture.md §Banner Discipline](../_bmad-output/planning-artifacts/architecture.md)
for the canonical text.

## RFC 2119 discipline

In `spec/`, uppercase MUST / MUST NOT / SHOULD / SHOULD NOT / MAY are
reserved for normative statements. In prose where you mean "should" as
advice, use lowercase — `spec-lint.yml` flags misuse.

`spec/` documents are self-contained: a `grep 'see lib/' spec/*.md` in CI
will fail. Never cite implementation as normative authority.

## PR template

The [PR template](.github/PULL_REQUEST_TEMPLATE.md) includes:
- A telemetry affirmation (zero client-side telemetry — see Cat 9).
- A banner check acknowledgement.
- Scope statement (which top-level directory is affected).

Keep PRs scoped to one top-level directory where possible. Cross-directory
changes (e.g. a lib API change that requires a spec update) are fine but
should split the diff by commit so review can route per-CODEOWNER.

## Versioning

Each top-level directory carries its own `VERSION`, `CHANGELOG.md`, and
release tag prefix (`spec-vX.Y.Z`, `lib-vX.Y.Z`, `server-vX.Y.Z`,
`conformance-vX.Y.Z`). See `lib/docs/versioning.md` for the compat matrix.

## Issues

Use the appropriate template under `.github/ISSUE_TEMPLATE/`:
- `errata.md` — normative spec defects.
- `bug-lib.md` — libteleproto3 defects.
- `bug-server.md` — server defects.
- `conformance-report.md` — external implementation conformance reports.
