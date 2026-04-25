# Security Policy

teleproto3 is an anti-censorship project. We take security reports seriously.

## Reporting a vulnerability

**Do not open a public issue for suspected vulnerabilities.**

Email: **security@teleproto3.invalid** (PGP key fingerprint TBD) — or use
GitHub's private vulnerability reporting flow on this repository.

Include:
- A description of the issue and its expected impact.
- Reproduction steps or a proof-of-concept.
- The affected component (`spec/`, `lib/`, `server/`, `conformance/`) and
  version tag where known.

We aim to acknowledge within **72 hours** and to provide a remediation plan
or public advisory timeline within **14 days** of acknowledgement.

## Scope

In scope:
- Protocol-level defects in `spec/` that weaken the threat model documented
  in [`spec/threat-model.md`](spec/threat-model.md).
- Implementation defects in `lib/` or `server/` that diverge from the spec
  in security-relevant ways.
- Conformance-harness false negatives that would let a non-conforming
  implementation pass.

Out of scope:
- Attacks explicitly covered by [`spec/non-goals.md`](spec/non-goals.md)
  (e.g. traffic-volume analysis, global passive adversary).
- Vulnerabilities in upstream MTProxy unrelated to Type3 — please report
  those upstream.
- Vulnerabilities in third-party client forks — report to the respective
  fork.

## Disclosure

We prefer coordinated disclosure. Once a fix is in a tagged release, we
publish a security advisory on the GitHub Security tab with CVE assignment
where appropriate.
