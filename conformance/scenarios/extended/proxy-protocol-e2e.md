---
status: draft
last_updated: 2026-04-27
---

> **Normative.** This document defines required behaviour for conforming
> Type3 implementations. Code in `lib/` is illustrative; where this
> document and `lib/` differ, this document wins.

# Scenario: <!-- ban-list-doc: technical identifier -->PROXY-Protocol<!-- /ban-list-doc --> End-to-End Test

**Compliance level:** extended  
**Owner:** Epic 2 story 2.10. This scaffold reserves the path; do NOT populate here.  
**Status:** draft — scenario body pending; path reserved by Story 1.9.

## Note on classification

This scenario lives in `extended/` (NOT `mandatory/`) because the
<!-- ban-list-doc: technical identifier -->PROXY-protocol<!-- /ban-list-doc --> v1/v2 path is operator-deployment-shape-specific: it is
only operative when a CDN or load balancer is in front of the Type3
server (per Epic 1 §4 and Epic 2 §11 ownership table). An IUT that does
not operate behind a CDN MAY skip this scenario and still claim Full
compliance, provided it declares the limitation in its conformance report.

## Purpose

Owned by Epic 2 story 2.10. This scaffold reserves the path; do NOT
populate here.

When populated by story 2.10, this scenario will validate:

1. <!-- ban-list-doc: technical identifier -->PROXY-protocol<!-- /ban-list-doc --> v1 header parsing (TCP4/TCP6).
2. <!-- ban-list-doc: technical identifier -->PROXY-protocol<!-- /ban-list-doc --> v2 binary header parsing.
3. Correct real-client-IP attribution after <!-- ban-list-doc: technical identifier -->PROXY-protocol<!-- /ban-list-doc --> decode.
4. Silent-close on <!-- ban-list-doc: technical identifier -->PROXY-protocol<!-- /ban-list-doc -->-malformed connections (anti-probe gate).

## Vectors consumed

_TBD by Epic 2 story 2.10._

## IUT pre-conditions

_TBD by Epic 2 story 2.10._

## Expected outcomes

_TBD — populated when Epic 2 story 2.10 lands._
