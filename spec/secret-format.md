---
spec_version: 0.1.0-draft
last_updated: 2026-04-25
status: draft
---

> **Normative.** This document defines required behaviour for conforming
> Type3 implementations. Code in `lib/` is illustrative; where this
> document and `lib/` differ, this document wins.

# Secret Format

Defines the Type3 secret encoding: the opaque blob a client receives
(via QR, deeplink, or paste) that carries everything needed to reach
the server.

The key words **MUST**, **MUST NOT**, **REQUIRED**, **SHALL**, **SHALL
NOT**, **SHOULD**, **SHOULD NOT**, **RECOMMENDED**, **MAY**, and
**OPTIONAL** in this document are to be interpreted as described in
[RFC 2119](https://www.rfc-editor.org/rfc/rfc2119) when, and only when,
they appear in all capitals.

## 1. Layout

A Type3 secret is a contiguous octet sequence with the following exact
byte layout:

```
offset  length    field
------  ------    ----------------------------------------------
0       1 octet   marker = 0xff
1       16        key (raw octets, no encoding)
17      ≥ 1       domain (UTF-8 octets, no terminator, no length
                  prefix; runs to end-of-buffer)
```

The total length of a well-formed secret is `total_len = 17 +
domain_len`, where `domain_len ≥ 1`. Equivalently, the minimum total
length is **18 octets** (1 marker + 16 key + at least 1 domain octet).

The marker octet **MUST** be exactly `0xff`. A buffer beginning with
any other octet is not a Type3 secret and **MUST** be rejected by §2.

The key field is exactly 16 octets. Producers **MUST NOT** insert
padding, length prefixes, or terminators between the marker and the
key, between the key and the domain, or after the domain.

Domain bytes are preserved verbatim. Producers **MUST NOT**
canonicalise, normalise (NFC/IDNA), case-fold, or otherwise transform
domain bytes during serialisation. Parsers **SHOULD** preserve raw
domain bytes verbatim (see §2).

`domain_len` is derived from the buffer length: `domain_len = total_len
- 17`. There is no separate length field. There is no terminator.

The result-shape contract for a successfully parsed secret is fixed
in §5; downstream sections cite §5 for vector schema.

## 2. Parsing

A conforming parser exposes the operation:

```c
t3_result_t t3_secret_parse(const uint8_t *buf, size_t len, t3_secret_t **out);
```

The argument order `(buf, len, out)` is fixed by `lib/include/t3.h` and
reproduced here for clarity; the spec is normative for behaviour, the
header is normative for signature.

### 2.1 Acceptance and rejection rules

A conforming parser **MUST**:

- Reject with **`MALFORMED`** when `len < 18`.
- Reject with **`MALFORMED`** when `buf[0] != 0xff`.
- Reject with **`MALFORMED`** when the octets in `buf[17 .. len-1]` do
  not form a valid UTF-8 sequence (any decoding error: lone
  continuation bytes, truncated multi-byte sequences, invalid lead
  bytes, overlong encodings, or surrogate-range scalar values).
- On success, populate the opaque `t3_secret_t *out` with the parsed
  key (16 octets) and domain (octet sequence and length).

A conforming parser **MUST**:

- Reject with **`INVALID_ARG`** when `out == NULL`.
- Reject with **`INVALID_ARG`** when `buf == NULL` and `len > 0`.
  (`buf == NULL && len == 0` is treated as a zero-length input and
  rejected with `MALFORMED`, not `INVALID_ARG`.)

A conforming parser **MUST NOT**:

- Modify the input buffer.
- Allocate from a buffer whose lifetime is tied to `buf`; the returned
  `t3_secret_t` **MUST** own copies of the key and domain octets.
- Return the spec-reserved error class **`INTERNAL`** for any input
  defined in this document. `INTERNAL` is reserved for implementation
  defects (heap exhaustion, broken invariants); see §6.

A conforming parser **SHOULD**:

- Preserve raw domain bytes verbatim. v0.1.0 parsers **SHOULD NOT**
  perform NFC, NFD, IDNA (Punycode), or case normalisation on the
  domain octets. (See §6 for the deferred normalisation proposal.)

### 2.2 Trailing-octet handling and version markers

At v0.1.0, the entire region `buf[17 .. len-1]` is the domain. A
v0.1.0-conformant parser **MUST** treat all post-key octets as domain
content; it **MUST NOT** attempt to demarcate trailing extension
payloads, because no future-version marker has been established at
v0.1.0.

Forward-compatibility behaviour for trailing extension octets, once a
future-version marker is established, is governed normatively by §4.

### 2.3 Error class catalogue

§2 introduces:

- **`MALFORMED`** — length, marker, or UTF-8 violations as enumerated
  in §2.1.
- **`INVALID_ARG`** — `NULL` output pointer or `NULL` input with
  positive length.

§4 introduces:

- **`UNSUPPORTED_VERSION`** — emitted only after a future-version
  marker is established and a v0.1.0 parser encounters it. Not
  reachable from any v0.1.0 input.

`INTERNAL` is implementation-reserved (see §6).

## 3. Serialisation

A conforming producer emits exactly the canonical form defined in §1:

1. One octet `0xff`.
2. Exactly 16 key octets.
3. The UTF-8 domain octets, preserved verbatim from the source.

Producers **MUST NOT** emit padding, length prefixes, or
terminators. Producers **MUST NOT** apply NFC/IDNA normalisation,
case-folding, or any other transformation to the domain octets.

### 3.1 Round-trip property

For every valid `t3_secret_t s`:

```
parse(serialise(s)) == s
```

Equality is defined component-wise: marker matches (both are `0xff`),
key octets are byte-equal, and domain octets are byte-equal.

A conforming implementation **MUST** satisfy this property for every
secret that round-trips through its own serialise→parse path. The
property is exercised by the conformance vectors in §5.

## 4. Backup-list

This section pins the v0.1.0 *parser-acceptance contract* for the
operator-signed backup-list extension (architecture row 5,
"Operator-signed backup-list data model"). The full signature scheme,
wire encoding, and verification semantics are listed in §6 Contested
Decisions and slated for normative promotion at v0.2.0.

### 4.1 v0.1.0 parser obligations

When (and only when) a future spec revision establishes a version
marker that demarcates the trailing-extension region from the v0.1.0
domain region:

- v0.1.0 parsers **MUST accept (tolerate trailing octets)** that
  follow the v0.1.0 layout. A v0.1.0 parser **MUST NOT** reject a
  longer secret solely because it carries trailing octets after a
  recognised future-version marker.
- v0.1.0 parsers **MUST NOT** trust unsigned entries in any
  trailing-extension region. A v0.1.0 parser that does not implement
  signature verification **MUST** treat extension contents as opaque
  and discard them after acceptance.
- v0.1.0 parsers **MUST NOT** propagate extension contents into the
  parsed secret's host or key fields. The `t3_secret_t.domain` field
  always reflects the v0.1.0 domain region; the `t3_secret_t.key`
  field always reflects the 16-octet key region.

Until a future-version marker is established, this section is dormant:
no input is recognised as a backup-list-bearing secret, and the entire
post-key region is the domain (§1, §2).

### 4.2 Forward citation

The signature format, the backup-list wire encoding, peer rotation
semantics, and the corresponding `extension_blob` activation are
deferred to v0.2.0 and tracked in §6.

## 5. Test vectors

This section binds the Type3 secret-format contract to the
machine-verifiable conformance vectors in
[`../conformance/vectors/unit.json`](../conformance/vectors/unit.json)
section `secret-format`.

### 5.1 Vector envelope

Every vector in `secret-format` follows the canonical envelope:

```json
{
  "id":     "<stable-unique-id>",
  "args":   ["<hex-string>"],
  "expect": { "ok": <bool>, "result"?: <object>, "error"?: "<CLASS>" }
}
```

`args[0]` is a lowercase hexadecimal string. The verifier decodes it
to octets and computes `len = len(args[0]) / 2`. Odd-length hex is a
vector-authoring error: the verifier **MUST** exit with code 2
(harness setup error). Empty string `""` decodes to `len = 0` and is
not a setup error; it is a valid `MALFORMED` rejection probe.

The verifier invokes `t3_secret_parse(buf, len, out)` and compares the
parser's result against `expect`.

### 5.2 Result schema

On success (`expect.ok == true`):

| Field                  | Type     | Required | Description                                                                                  |
|------------------------|----------|----------|----------------------------------------------------------------------------------------------|
| `result.key`           | string   | yes      | Exactly 32 lowercase hex characters (16 raw key octets).                                     |
| `result.domain`        | string   | yes      | UTF-8 string copied verbatim from the parsed domain bytes (no normalisation, no escaping).   |
| `result.extension_blob`| string   | no       | Lowercase hex of trailing octets after the v0.1.0 layout. Absent at v0.1.0; reserved for v0.2.0. |

On failure (`expect.ok == false`):

| Field           | Type     | Required | Description                                                          |
|-----------------|----------|----------|----------------------------------------------------------------------|
| `expect.error`  | string   | yes      | One of the spec-conformant classes: `MALFORMED`, `INVALID_ARG`, `UNSUPPORTED_VERSION`. |

Vectors **MUST NOT** assert `error: "INTERNAL"`. `INTERNAL` is
implementation-reserved (§6).

### 5.3 XFAIL-PENDING-1.7

Until the conformance test runner is materialised by Story 1.7,
vectors in `conformance/vectors/unit.json :: secret-format` are
**XFAIL-PENDING-1.7**. Story 1.1 deliverables are spec text + vector
data; the runner that exercises them (and any `xfail` mechanism) is
owned by Story 1.7. The vector file carries an `_xfail_pending`
documentation key alongside `_scaffold_note` to signal this state to
the runner.

### 5.4 Vector inventory

The `secret-format` array contains 11 vectors at v0.1.0:

| ID                              | Polarity | Purpose                                                                                     |
|---------------------------------|----------|---------------------------------------------------------------------------------------------|
| `well-formed-v1`                | accept   | Minimal happy path with ASCII domain.                                                       |
| `well-formed-len-18-min`        | accept   | Boundary: minimum well-formed length (18 octets, 1-octet domain).                           |
| `well-formed-non-ascii-cyrillic`| accept   | Non-ASCII UTF-8 domain (Cyrillic IDN), locks "preserve verbatim" rule.                      |
| `malformed-len-17`              | reject   | Boundary: total length 17 (zero domain octets).                                             |
| `malformed-len-zero`            | reject   | Empty buffer.                                                                               |
| `malformed-prefix-fe`           | reject   | Wrong marker octet (`0xfe`).                                                                |
| `malformed-domain-bad-utf8`     | reject   | Invalid UTF-8 in domain (lone continuation byte).                                           |
| `malformed-domain-truncated-utf8` | reject | Multi-byte UTF-8 sequence cut short at end-of-buffer.                                       |
| `trailing-extension-len-4`      | accept   | v0.1.0 trailing-extension tolerance (4-octet trailing region; v0.2.0 forward-simulation).   |
| `trailing-extension-len-16`     | accept   | v0.1.0 trailing-extension tolerance (16-octet trailing region; v0.2.0 forward-simulation).  |
| `trailing-extension-len-64`     | accept   | v0.1.0 trailing-extension tolerance (64-octet trailing region; v0.2.0 forward-simulation).  |

The three `trailing-extension-*` vectors satisfy the AC-2 backup-list
clause (vectors covering "secret with backup list of N=1/2/3") at
v0.1.0. Because §4 defers the future-version marker to v0.2.0, the
post-key trailing region is — at v0.1.0 — interpreted entirely as
domain octets; the trailing payloads are shaped so v0.2.0
reinterpretation will split them into `domain` + `extension_blob`
without re-authoring the vector file. See §4 and §6.

## 6. Contested Decisions

This section records design choices that were considered and either
deferred or rejected. Nothing in this section is normative at v0.1.0;
items marked "v0.2.0" are scheduled for normative promotion in the
next minor revision and are listed here so that downstream
implementations can plan ahead.

### 6.1 Domain normalisation (deferred → v0.2.0)

**Proposal:** apply NFC or IDNA-2008 (Punycode) normalisation to
domain octets during parse and serialise.

**Decision (v0.1.0):** rejected for v0.1.0. Normalisation breaks
bit-exactness with secrets generated by simple producers and would
cause `parse(serialise(s)) != s` for any domain whose canonical form
differs from its source form. The §3 round-trip property is
load-bearing for conformance vector authoring.

**Forward path:** revisit at v0.2.0 alongside the backup-list
extension, where normalisation can be specified at the
extension-payload boundary without affecting the v0.1.0 domain bytes.

### 6.2 Explicit domain length prefix (rejected)

**Proposal:** prepend a 1- or 2-octet length field before the domain
to remove the implicit `domain_len = total_len - 17` calculation.

**Decision:** rejected. The implicit derivation is unambiguous (§1
fixes the layout), zero-cost on the wire, and unblocks the
trailing-extension forward-compat path of §4. An explicit length
prefix would force a breaking change at v0.2.0 to introduce extension
trailing bytes.

### 6.3 RFC 1035 253-octet domain ceiling (rejected)

**Proposal:** cap `domain_len` at 253 octets per RFC 1035.

**Decision:** rejected. Domain values in Type3 secrets are not DNS
hostnames in the strict sense; they may carry SNI hints, port
qualifiers, or future extension payloads (§4) that exceed 253 octets
legitimately. The parser **MUST** accept any positive `domain_len`
that does not violate other §2 rules.

### 6.4 Ed25519-SHA256 backup-list signature scheme (deferred → v0.2.0)

**Proposal:** define backup-list entries as length-prefixed records
with Ed25519 signatures over (`peer_endpoint || expiry_unix_seconds`)
and SHA-256 fingerprint pinning of the operator's signing key.

**Decision (v0.1.0):** deferred. The §4 parser-acceptance contract is
sufficient at v0.1.0 to ensure forward-compat. Pinning a specific
signature algorithm in the v0.1.0 release would over-commit before
implementer feedback is collected.

**Forward path:** v0.2.0 promotes a chosen scheme to normative status,
authors the matching wire encoding, and activates `extension_blob` as
an OPTIONAL parser output (§5.2).

### 6.5 base32 / base64 envelope wrappers (rejected)

**Proposal:** wrap the binary secret in a printable-character envelope
to ease pasting into chat or documentation.

**Decision:** rejected at the spec layer. Encoding for human transport
is the URL parser's responsibility (Story 1.2 covers `tg://` deeplink
and QR encoding). The secret-format spec defines the binary blob; how
it is presented at the UI surface is out of scope.

### 6.6 `INTERNAL` error class (implementation-reserved)

`t3_result_t` includes an `INTERNAL` enumerant for implementations to
signal unrecoverable defects (heap exhaustion, broken invariants,
panics translated to errors). A spec-conformant parser **MUST NOT**
return `INTERNAL` for any input defined in this document, and
conformance vectors **MUST NOT** assert `error: "INTERNAL"`.

`INTERNAL` exists in the symbol table for implementation use only; it
is never spec-mandated and never test-asserted. This entry is recorded
here so that a future contributor reading `t3.h` can see why
`INTERNAL` is absent from the §2 error catalogue.
