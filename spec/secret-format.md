---
spec_version: 0.1.0-draft
last_updated: 2026-04-25
status: draft
amendment_log:
  - id: A-003
    date: 2026-04-25
    summary: Destination structure — host[/path] split inside domain field (§1.1)
  - id: A-004
    date: 2026-04-25
    summary: Length-silence — no upper bound on domain_len / host_len / path_len (§1.1, §6.3, §6.8)
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
17      ≥ 1       domain = <host>[<path>] (UTF-8 octets, no
                  terminator, no length prefix; runs to
                  end-of-buffer; structure defined in §1.1)
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

### 1.1 Destination structure

The `domain` field carries a destination string with the structure
`<host>[<path>]`, where `<host>` and `<path>` are derived from the
`domain` octets by a fixed split rule. Let `D` denote the `domain`
octet sequence (i.e. `buf[17 .. len-1]`) and `0x2f` denote the
ASCII `/` octet.

- If `D` contains no `0x2f` octet: `host = D`, `path = ""` (empty).
- If `D[0] == 0x2f`: leading `/` without a host. The buffer is
  rejected by §2 with **`MALFORMED`**.
- Otherwise let `i` be the smallest index with `D[i] == 0x2f`;
  then `host = D[0..i)` and `path = D[i..end]` (path includes its
  leading `/`).

The split is performed on the **first** `0x2f` only. Any further
`0x2f` octets are part of `path` and are not interpreted further by
this layer.

A trailing `0x2f` (i.e. `path` consisting solely of the leading
slash, `path = "/"`) is well-formed and yields a non-empty `path`
component. The round-trip identity `domain == host || path` (e.g.
`"example.com/" == "example.com" || "/"`) holds trivially.

The two empty-path-like states are syntactically distinct: `path =
""` (no `0x2f` octet anywhere in `domain`) and `path = "/"` (a
single trailing `0x2f`) round-trip to different `domain` octets
(`"example.com"` vs `"example.com/"`). Routing layers **MUST NOT**
treat them as equivalent solely because both yield no path
segments; the canonical secret bytes differ. Consumers that need
to fold the two cases together for application logic (e.g. routing
keys) **SHOULD** do so above the parser layer; the parser
preserves the distinction.

The identity `domain == host || path` (octet concatenation) holds
for every well-formed secret; parsers **MUST** preserve it.

Both `host` and `path` octets are preserved verbatim. Producers and
parsers **MUST NOT** apply percent-encoding, percent-decoding,
case-folding, NFC/NFD/IDNA normalisation, or any other
transformation to either field. (See §3 for producer rules; §6.1
for the deferred normalisation discussion.)

The spec imposes **no upper bound** on `domain_len`, `host_len`, or
`path_len` beyond the §1 constraint `domain_len ≥ 1`. Rejected
length-related proposals are recorded in §6.3 (length cap, RFC 1035
253-octet ceiling) and §6.8 (length-prefixed path); producers
seeking practical guidance for transport-layer limits (URL paste,
QR encoding, HTTP request line) consult installer / operator docs,
not this spec.

By construction (§2.1 leading-`0x2f` rejection plus §1's `domain_len
≥ 1` constraint), `host_len ≥ 1` for every successfully parsed
secret; `host` is never empty.

The `domain` field is preserved on every parsed result for backward
compatibility with consumers that read `result.domain`. New
consumers **SHOULD** read `result.host` and `result.path` directly.
See §5.2 for the full result schema.

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
  not form a valid UTF-8 sequence per RFC 3629 §3. This includes
  lone continuation bytes, truncated multi-byte sequences, invalid
  lead bytes, overlong encodings, and the 3-octet sequences in the
  range `ED A0 80` to `ED BF BF` that encode UTF-16 surrogate code
  points (U+D800..U+DFFF). UTF-8 itself cannot natively encode
  surrogate code points; the cited 3-octet range is the CESU-8 /
  WTF-8 form RFC 3629 §3 forbids.
- Reject with **`MALFORMED`** when `D[0] == 0x2f` — i.e. `domain`
  begins with `/` without a host prefix. See §1.1 for the definition
  of `D`.
- On success, populate the opaque `t3_secret_t *out` with the parsed
  key (16 octets), the full `domain` (octet sequence and length),
  and the destination split derived per §1.1: `host` (the `domain`
  octets preceding the first `0x2f`, or the entire `domain` if none
  is present) and `path` (the `domain` octets from the first `0x2f`
  to end-of-buffer, or the empty octet sequence if no `0x2f` is
  present).

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

When the source holds `host` and `path` separately (per §1.1),
producers **MUST** construct `domain` by octet concatenation
`domain = host || path`. When `path` is empty, `domain == host`.
When `path` is non-empty, its first octet **MUST** be `0x2f`
(`/`); producers **MUST NOT** emit a non-empty `path` whose first
octet is anything other than `0x2f`.

Producers **MUST NOT** emit a `host` containing `0x2f`. The first
`0x2f` octet in `domain` is reserved as the host/path boundary per
§1.1; embedding `0x2f` inside `host` would round-trip-collapse to a
different `(host, path)` pair after parse, violating §3.1.

Producers **MUST NOT** emit an empty `host`. By §1.1's split rule
plus §2.1's leading-`0x2f` rejection, an empty `host` would force
`domain` to begin with `0x2f` (when `path` is non-empty) and §2.1
rejects such buffers as `MALFORMED`. When the source has no host,
the secret is not well-formed and **MUST NOT** be serialised.

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

### 4.3 Forward-compatibility constraint on the version marker

Future spec revisions establishing a version marker (per §2.2 / §4.1)
**MUST NOT** select the octet `0x2f` (ASCII `/`) as the marker value.
A `0x2f` marker would collide with §1.1's destination-structure split
rule: v0.1.0 parsers (which have no awareness of the marker) would
treat the marker octet as the host/path boundary and silently
misroute requests. This constraint is an obligation on the future
v0.2.0+ amendment author at marker-selection time; v0.1.0
implementations cannot enforce it and are not expected to.

The companion rejection of length-prefixed paths in §6.8 is one
illustration of why the v0.1.0 trailing region must remain
uncluttered for v0.2.0 evolution; this constraint is the symmetric
obligation on the v0.2.0 marker selection.

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
| `result.domain`        | string   | yes      | UTF-8 string copied verbatim from the parsed domain bytes (no normalisation, no escaping). Equals `host || path` (octet concatenation; see §1.1). Preserved for backward-compat with v0.1.0-draft consumers (see Consumer guidance below). |
| `result.host`          | string   | yes      | UTF-8 string copied verbatim from the host portion of the domain (octets preceding the first `0x2f`, or the entire domain if no `0x2f` is present). Never empty. See §1.1. |
| `result.path`          | string   | yes      | UTF-8 string copied verbatim from the path portion of the domain (octets from the first `0x2f` to end-of-buffer inclusive of the leading `/`, or the empty string if no `0x2f` is present). May be empty. See §1.1. |
| `result.extension_blob`| string   | no       | Lowercase hex of trailing octets after the v0.1.0 layout. Absent at v0.1.0; reserved for v0.2.0. |

**Consumer guidance.** New consumers **SHOULD** read `result.host`
and `result.path` directly. The `result.domain` field is preserved
for backward compatibility with v0.1.0-draft consumers written
before A-003. Consumers that route, validate, or display `domain`
without splitting on the first `0x2f` may mis-attribute path bytes
to host (e.g. mis-targeted SNI, mis-scoped certificate validation,
mis-rendered host UI); the failure mode is consumer-specific and
depends on which downstream layer consumes `domain` directly. See
§1.1 for the split contract.

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

The `secret-format` array contains 16 vectors at v0.1.0 (11 baseline +
5 destination-structure vectors added by amendment A-003):

| ID                              | Polarity | Purpose                                                                                     |
|---------------------------------|----------|---------------------------------------------------------------------------------------------|
| `well-formed-v1`                | accept   | Minimal happy path with ASCII domain (host-only, empty path).                               |
| `well-formed-len-18-min`        | accept   | Boundary: minimum well-formed length (18 octets, 1-octet domain; host-only, empty path).    |
| `well-formed-non-ascii-cyrillic`| accept   | Non-ASCII UTF-8 domain (Cyrillic IDN), locks "preserve verbatim" rule.                      |
| `malformed-len-17`              | reject   | Boundary: total length 17 (zero domain octets).                                             |
| `malformed-len-zero`            | reject   | Empty buffer.                                                                               |
| `malformed-prefix-fe`           | reject   | Wrong marker octet (`0xfe`).                                                                |
| `malformed-domain-bad-utf8`     | reject   | Invalid UTF-8 in domain (lone continuation byte).                                           |
| `malformed-domain-truncated-utf8` | reject | Multi-byte UTF-8 sequence cut short at end-of-buffer.                                       |
| `trailing-extension-len-4`      | accept   | v0.1.0 trailing-extension tolerance (4-octet trailing region; v0.2.0 forward-simulation).   |
| `trailing-extension-len-16`     | accept   | v0.1.0 trailing-extension tolerance (16-octet trailing region; v0.2.0 forward-simulation).  |
| `trailing-extension-len-64`     | accept   | v0.1.0 trailing-extension tolerance (64-octet trailing region; v0.2.0 forward-simulation).  |
| `well-formed-host-and-path`           | accept | A-003: host with single-segment path (e.g. `example.com/ws/abc123`); locks first-`/` split.                |
| `well-formed-host-and-deep-path`      | accept | A-003: host with multi-segment path; locks "subsequent `/` stay inside path" rule.                         |
| `well-formed-idn-host-and-path`       | accept | A-003 (review fold-in P-8): non-ASCII (Cyrillic IDN) host with path; locks first-`/` under multi-byte UTF-8.|
| `well-formed-host-and-trailing-slash` | accept | A-003 (review fold-in P-14): trailing `/` (path consisting solely of `/`); locks §1.1 trailing-slash rule. |
| `malformed-leading-slash`             | reject | A-003: `domain` begins with `/` (no host prefix); locks §2.1 leading-slash MUST rejection.                 |

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

### 6.7 Path as a separate URL parameter (rejected)

<!--
  ban-list-doc sentinel (Story 1.1 / A-003): the enclosed prose
  documents a rejected URL-scheme variant. The literal Telegram
  deeplink scheme inside this block contains the AR-C1 ban-list
  token "proxy" by necessity (it is the historical name of the
  v1 scheme); the sentinel signals AR-C1 banlist tooling (owned by
  Story 1.4) to treat hits between the opening and closing markers
  as documentary, not user-facing copy. Format (case-sensitive):

    <!-- ban-list-doc: <reason> -->
    ...prose containing banned token...
    <!-- /ban-list-doc -->

  The opening marker MUST start with the literal "ban-list-doc:"
  and the closing marker MUST be exactly "/ban-list-doc". Story
  1.4 will codify this contract in the linter.
-->
<!-- ban-list-doc: A-003 §6.7 documents a rejected URL-scheme variant; the literal Telegram deeplink scheme is retained for unambiguous reference -->
**Proposal:** carry the upgrade path as a separate URL-level field
in the `tg://proxy?…` deeplink, e.g.
`tg://proxy?server=example.com&port=443&secret=<HEX_SECRET>&path=/ws/abc123`,
leaving the secret payload at the v0.1.0 `0xff + key + host` shape.
(`<HEX_SECRET>` is a placeholder for the lowercase-hex Type3 secret;
it is not part of the example and **MUST NOT** be interpreted as a
literal value.)

**Decision (A-003, 2026-04-25):** rejected. Splitting the
destination across two parsing layers (URL parser + secret parser)
fragments rotation-detection logic, allows tampering with `path` at
the URL layer without invalidating the 16-byte key, and does not
extend cleanly to QR / BLE / paste flows where the secret is the
atomic unit of trust. Inlining `path` inside `domain` (§1.1) makes
the secret self-contained: the same byte sequence works across
every transport.
<!-- /ban-list-doc -->

### 6.8 Length-prefixed path (rejected)

**Proposal:** structure `domain` as
`<host-utf8> || 0x00 || <path-len:u8> || <path-utf8>`, with an
explicit zero-octet host terminator and a one-octet path length
field, instead of the implicit first-`/` split of §1.1.

**Decision (A-003, 2026-04-25):** rejected. The implicit `/` split
is unambiguous, zero-cost on the wire, and consistent with §6.2's
prior rejection of an explicit `domain_len` prefix. An explicit
path-length field would also conflict with §4's
trailing-extension forward-compat path by fixing the path region
inside `domain`, leaving no contiguous tail for v0.2.0 extension
payloads. The implicit split is constant-time to derive and adds
no parser state.
