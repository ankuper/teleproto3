<!--
Thanks for contributing to teleproto3. Fill out the relevant sections
below. CI will check banner discipline, RFC 2119 usage, and telemetry
affirmation automatically — but please confirm manually too.
-->

## Scope

<!-- Which top-level directory is affected? Keep to one where possible. -->
- [ ] `spec/`
- [ ] `lib/`
- [ ] `server/`
- [ ] `conformance/`
- [ ] `.github/` or root meta
- [ ] Cross-directory (explain why below)

## Summary

<!-- What changes and why. Link related issues / errata. -->

## Banner check

- [ ] If this PR adds or modifies files under `lib/src/`, each modified
      file begins with the reference-implementation banner.
- [ ] If this PR adds or modifies files under `spec/`, each modified file
      begins with the normative frontmatter + banner and uses RFC 2119
      keywords correctly.

## Telemetry affirmation (Cat 9)

- [ ] I confirm this PR adds **zero** client-side telemetry, beacons, or
      third-party SDKs to any client-facing component. Server-local admin
      observability (logs, counters) is allowed; client-side is not.

## Version impact

<!-- Does this require a VERSION bump? If so, which top-level directory's
     VERSION and why (MAJOR/MINOR/PATCH)? -->

## Test plan

<!-- How did you verify? Unit tests, conformance scenarios, manual
     tcpdump, etc. -->
