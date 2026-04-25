# Interop notes & platform gotchas

Collected during the spec-v0.1.0 drafting epic and early consumer
integrations. Updated as new gotchas surface.

## Safari 15

_TBD._ Safari 15 and earlier have WebSocket mask / compression quirks
that can manifest as "frames look valid server-side but iOS client
times out". Document the symptom, the workaround, and the Safari
version range.

## Old nginx

_TBD._ Pre-1.20 nginx WebSocket proxy forwarding behaves differently
on `proxy_read_timeout` than 1.24+. Recommended minimum nginx version
and the config knobs that matter live in
[`../../server/docs/deployment.md`](../../server/docs/deployment.md).

## Cloudflare Worker

_TBD._ Workers cap WebSocket message size; long frames may be
fragmented more aggressively than bare nginx. Implementations MUST
handle arbitrary fragmentation per RFC 6455 §5.4 — the reassembly
invariant applies.

## Android TLS

_TBD._ Older Android versions' default TrustManager chains don't
include some CAs that nip.io domains on LetsEncrypt issue against.
Recommend pinning or shipping an up-to-date CA bundle.

## iOS Bazel

_TBD._ The iOS fork consumes lib/ via Bazel `http_archive`. If the
SHA256 pin mismatches after a publish, the fork's CI fails closed —
that's intentional. Never force-push a published `lib-v*` tag; cut
a new patch instead.
