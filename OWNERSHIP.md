# Ownership & Governance

## Maintainer

- **@ankuper** — primary maintainer, all top-level directories.

Per-directory CODEOWNERS pointers live in [`.github/CODEOWNERS`](.github/CODEOWNERS) and
are the source of truth for PR review routing.

## Errata SLA

Normative errata filed against `spec/` via the [errata issue template](.github/ISSUE_TEMPLATE/errata.md)
receive a first-response acknowledgement within **30 days**. Acknowledgement
does not imply agreement — it confirms the issue has been triaged and is
tracked.

Non-normative issues (implementation bugs, docs nits, conformance report
submissions) follow best-effort response.

## Decision-making

- **Spec changes** (`spec/`): breaking changes require a MAJOR version bump and
  a written rationale in `spec/CHANGELOG.md`. Additive clarifications go through
  the errata process first, then fold into the next MINOR.
- **Library changes** (`lib/`): must not violate the spec. If lib and spec
  disagree, spec wins — see banner discipline.
- **Server changes** (`server/`): upstream MTProxy changes merged via
  `git subtree pull`; fork-local changes documented in `server/UPSTREAM.md`.

## Succession

If primary maintainer becomes unavailable for >90 days, repo access transfers
according to a dormant-project policy to be documented separately. Until then,
there is a single point of failure — this is an honest risk.
