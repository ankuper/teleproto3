---
spec_version: 0.1.0-draft
last_updated: 2026-04-27
status: draft
amendment_log:
  - id: W-001
    date: 2026-04-27
    summary: §4 obfuscated stream lifted from ERRATA-deferred to normative — AES-256-CTR / KDF formulae inline, magic-tag admission gate, MUST-NOT-derive-from-IP, MUST-CTR-continuous, SHOULD-zeroise-secret
  - id: W-002
    date: 2026-04-27
    summary: Round-8 close-out — D2 flat MALFORMED, D3 (cmd,version) dispatch table, D4 subprotocol MUST-NOT-echo, D6 HTTP/1.1 hard-pin, GET-target structured (host,path) rule, EOF-during-fragmentation contract, idle-session accept
---

> **Normative.** This document defines required behaviour for conforming
> Type3 implementations. Code in `lib/` is illustrative; where this
> document and `lib/` differ, this document wins.

# Wire Format

The end-to-end shape of a Type3 connection: TLS → WebSocket upgrade →
Session Header → obfuscated-2 MTProto. This is the document
implementers live in.

## 1. Connection establishment

A Type3 connection is layered as TLS → WebSocket upgrade → Session
Header → obfuscated-2 MTProto. This section is normative for the lower
two layers (TLS + WS upgrade); §3 defines the Session Header that
follows.

**TLS layer.** TLS terminates externally to the Type3 protocol. This
specification is host-stack-neutral (NFR-S): it does not name a TLS
implementation, and conforming endpoints MAY use any TLS 1.2 or TLS
1.3 stack provided by their host environment. The bytes carried inside
the TLS `application_data` record are a standards-compliant WebSocket
stream per RFC 6455.

**WebSocket upgrade.** The client initiates a single HTTP/1.1 `GET`
request. v0.1.0 hard-pins HTTP/1.1 as the carrier; HTTP/2 extended
CONNECT (RFC 8441) and HTTP/3 extended CONNECT (RFC 9220) are RESERVED
for v0.2 and MUST NOT be used by a v0.1.0 conforming endpoint.

The HTTP/1.1 GET request target is constructed from the parsed secret
(`secret-format.md` §1.1) as:

```
GET /<path>  HTTP/1.1                if result.path != ""
GET /        HTTP/1.1                if result.path == ""
Host: <host>
```

where `<host> = result.host` and `<path> = result.path`. Equivalent in
identity terms to using `result.domain` split at the first `0x2f`,
but implementations MUST use the structured `(host, path)` pair, NOT
the unsplit `domain` field, to construct the GET target.

The request MUST carry the canonical RFC 6455 upgrade headers:

```
Host: <host>
Connection: Upgrade
Upgrade: websocket
Sec-WebSocket-Version: 13
Sec-WebSocket-Key: <16 random octets, base64-encoded>
```

The 512-byte ceiling on the `domain` field (`secret-format.md` §2.1
MALFORMED rule 3, A-005) implicitly bounds both `Host:` and the GET
target; v0.1.0 endpoints MAY rely on this ceiling to size HTTP-line
buffers.

Additional headers (`Origin`, `User-Agent`, etc.) MAY be present and
carry no Type3-level semantics; the server MUST NOT reject solely on
their content. A client MAY send `Sec-WebSocket-Protocol`; the server
MUST ignore its content and MUST NOT echo `Sec-WebSocket-Protocol`
back in the `101` response (no subprotocol is negotiated at v0.1.0).

The server completes the upgrade with a `101 Switching Protocols`
response carrying:

```
Upgrade: websocket
Connection: Upgrade
Sec-WebSocket-Accept: <base64(SHA1(Sec-WebSocket-Key || GUID))>
```

where `GUID` is the RFC 6455 magic string
`258EAFA5-E914-47DA-95CA-C5AB0DC85B11`. The server MUST NOT negotiate
any WebSocket extensions; the 101 response MUST omit
`Sec-WebSocket-Extensions` and MUST omit `Sec-WebSocket-Protocol`.
After the `101` response, both sides exchange WebSocket frames per §2;
the first 4 bytes of the WebSocket payload stream are the Session
Header per §3.

**Frontend layer (non-normative).** The server MAY be fronted by an
external L7 frontend (deployment example: `nginx`) that terminates TLS
on its behalf but MUST NOT strip, rewrap, or buffer WebSocket frames;
frames are handed through opaquely. The frontend MUST NOT downgrade or
upgrade the HTTP version of the upstream WebSocket connection (i.e.,
MUST present HTTP/1.1 to the Type3 endpoint regardless of the
client-facing version). This deployment shape is described in
<!-- ban-list-allow:doc-reference --> `docs/mtproxy3-architecture.md`
<!-- /ban-list-allow --> and is non-normative — conforming endpoints
do not depend on it. Direct TLS termination by the Type3 endpoint is
equally conforming (FR1, NFR-S).

See diagram [`diagrams/connection-flow.mmd`](diagrams/connection-flow.mmd).

## 2. WebSocket framing

After the §1 upgrade, both endpoints exchange WebSocket frames per RFC
6455. Type3 imposes the following constraints on top of RFC 6455:

**Error class for §2 violations.** A frame violating any §2
constraint below is `MALFORMED` per §5 and MUST trigger a silent
close. v0.1.0 distinguishes only two wire-observable error classes
(`MALFORMED` vs `UNSUPPORTED_VERSION`); finer sub-classification of
WebSocket framing violations is not normative. Implementations MAY
log the specific violation locally for operational visibility (e.g.
`bad_mask`, `bad_opcode`, `rsv_set`), but the on-wire response is
identical for every flavour.

**Masking.** Per RFC 6455 §5.3, client→server frames MUST set MASK=1
and carry a 4-byte masking key; server→client frames MUST set MASK=0.
A frame that violates either rule is `MALFORMED`.

**Opcode.** Type3 payload frames MUST use opcode `0x2` (Binary). When a
Binary message is fragmented per RFC 6455 §5.4, the initial frame
carries opcode `0x2` and subsequent continuation frames carry opcode
`0x0` (Continuation); the parser MUST accept both. Frames with opcode
`0x1` (Text) are `MALFORMED`. Frames with reserved or unrecognised
opcodes (values other than `0x0`–`0x2`, `0x8`–`0xA`) are `MALFORMED`.
Control frames (`0x8` Close, `0x9` Ping, `0xA` Pong) are passed to the
WebSocket layer below; the Type3 layer ignores them apart from
observing a Close, after which the underlying transport is torn down
without further Type3-level processing. Control frames MAY be
interleaved during Session Header reassembly; the receiver MUST NOT
discard accumulated header bytes upon receiving a control frame.

**Fragmentation.** A logical Type3 record (the 4-byte Session Header,
or a window of obfuscated-2 stream bytes) MAY span two or more
WebSocket frames; the parser MUST buffer until enough payload bytes
have been accumulated and then advance. In particular, a Session
Header MAY be delivered as `[2 bytes][2 bytes]`, `[1][1][1][1]`, or
any other partition summing to four bytes; the parser MUST NOT assume
frame-boundary alignment with logical record boundaries (FR1, FR2). A
single WebSocket frame MAY also carry the 4-byte Session Header
followed immediately by an arbitrary prefix of the obfuscated-2 stream;
parsers MUST consume exactly four bytes of header and then route the
remainder to the §4 stream consumer.

**EOF during Session Header reassembly.** If the underlying transport
closes (TCP FIN / TLS close_notify / WebSocket Close) before the
parser has accumulated four bytes of Session Header, the partial
header is `MALFORMED`. A receiver that has buffered zero header bytes
when EOF arrives MUST also treat the connection as `MALFORMED` for
local telemetry (no header was ever supplied); the §5 silent-close
contract still governs the wire response (i.e., no response bytes are
emitted regardless).

**Other RFC 6455 constraints unchanged.** RSV bits MUST be zero; an
unmasked client frame, a masked server frame, or any reserved-bit
non-zero frame is `MALFORMED`.

## 3. Session Header

The first 4 bytes of the WebSocket payload stream (after any
§2 fragmentation) form the AR-S1 Session Header. The header is
plaintext — it is not under obfuscated-2 (see §4).

**Byte layout.** The header is exactly 4 bytes:

```
offset  size  field         encoding
------  ----  -----         --------
  0      1    command_type  u8
  1      1    version       u8
  2      2    flags         u16 little-endian
```

Total 4 bytes. No magic prefix, no length field, no checksum.

**Command-type registry (v0.1.0).** Each row gives a sender duty (a
producer MUST NOT emit this byte) and a receiver duty (a v0.1.0 parser
MUST react this way on receipt). The two duties are stated separately
so future versions can promote a reserved slot without renumbering:

| Value       | Name                  | Sender duty (v0.1.0)                       | Receiver duty (v0.1.0)                                                          |
|-------------|-----------------------|--------------------------------------------|----------------------------------------------------------------------------------|
| `0x00`      | reserved              | MUST NOT emit                              | `MALFORMED`. Reserved as a sentinel for "no command type set"; symmetric with `0xFF`. |
| `0x01`      | `MTPROTO_PASSTHROUGH` | MUST emit for every v0.1.0 conn (FR1)      | Process the connection at v0.1.0 semantics (single normative value at v0.1.0).  |
| `0x02`      | `HTTP_DECOY_MIMIC`    | MUST NOT emit at v0.1.0                    | `MALFORMED` (reserved-not-allocated, FR3).                                       |
| `0x03–0xFE` | reserved              | MUST NOT emit at v0.1.0                    | `MALFORMED` (reserved for future allocation, FR3).                              |
| `0xFF`      | reserved              | MUST NOT emit                              | `MALFORMED`. Reserved as the upper boundary of the command-type space, symmetric with `0x00` (cf. `secret-format.md` §1, where `0xFF` marks the *secret* version sentinel — distinct field, distinct namespace). |

**Version field.** At v0.1.0 the `version` byte is `0x01`. A header
carrying `version > 0x01` triggers §6 Version Negotiation. A header
carrying `version == 0x00` is `MALFORMED`.

**Flags field.** `flags` is a 16-bit value transmitted in little-endian
byte order (low byte first). At v0.1.0 every bit MUST be zero; a
non-zero `flags` value is `MALFORMED`. Future versions MAY allocate
flag bits without bumping the transport ID (FR2): the `version` field
governs structural / parser-dispatch changes; `flags` governs
within-version feature extensions. The two axes are deliberately
separated (see §7).

The little-endian encoding is normative. Both examples below are
v0.1.0-`MALFORMED` (every flag bit MUST be zero); they illustrate
endianness only:

- A logical `flags = 0x0001` would serialise as bytes `0x01 0x00`.
- A logical `flags = 0x0100` would serialise as bytes `0x00 0x01`.

**Field validation precedence.** Fields are validated in order:
`command_type` → `version` → `flags`; the first failure determines the
returned error class. A header with multiple invalid fields returns the
error class of the earliest failing field. The (`command_type`,
`version`) error class table below resolves the corner where both
fields are simultaneously suspect:

| `command_type`     | `version`            | Returned error class    | Rationale                                                                  |
|--------------------|----------------------|-------------------------|----------------------------------------------------------------------------|
| `0x01` (known)     | `> 0x01` (unknown)   | `UNSUPPORTED_VERSION`   | Known command at unknown version — version negotiation per §6.            |
| `0x02–0xFE` (unallocated at v0.1.0) | `0x01` (known)       | `MALFORMED`             | Unknown command at known version — reserved slot, no future semantics.    |
| `0x02–0xFE` (unallocated at v0.1.0) | `> 0x01` (unknown)   | `UNSUPPORTED_VERSION`   | Future-recognised: unknown command at unknown version is forward-compat — defer to version negotiation. |
| `0x00` / `0xFF` (sentinel) | (any)        | `MALFORMED`             | Sentinel slots are MALFORMED regardless of version.                        |
| `0x01` (known)     | `0x00` (sentinel)    | `MALFORMED`             | Sentinel version slot is MALFORMED regardless of command type.            |

**Concrete vectors.** The following hex sequences illustrate each
disposition; matching machine-verifiable vectors live in
`conformance/vectors/unit.json` under section key `session-header`:

- Valid v1, no flags: `01 01 00 00` — `command_type = MTPROTO_PASSTHROUGH`, `version = 1`, `flags = 0`.
- `MALFORMED` (non-zero flags at v0.1.0): `01 01 01 00`.
- `MALFORMED` (unknown command type at v0.1.0): `02 01 00 00` — `0x02` is reserved-not-allocated.
- Forward version (triggers §6): `01 02 00 00` — a v0.1.0 server returns `UNSUPPORTED_VERSION` for telemetry and silent-closes.

See diagram [`diagrams/session-header.mmd`](diagrams/session-header.mmd).

## 4. Obfuscated stream (AES-256-CTR setup) <a id="W-001"></a>

The bytes following the 4-byte Session Header form the obfuscated-2
MTProto stream. Header bytes (offsets 0–3 of the WebSocket payload
stream) are NOT encrypted. The obfuscated-2 stream begins at byte 4
of that stream. WebSocket frame boundaries are independent of cipher
block boundaries and of MTProto frame boundaries: a parser MUST NOT
realign, re-segment, or pad the obfuscated-2 stream to fit WebSocket
frame boundaries; the stream is delivered to the cipher
byte-for-byte in arrival order across however many WebSocket frames
the underlying transport chose to emit (FR1).

### 4.1 Quick reference

After the §3 Session Header, the client MUST send 64 bytes of header
material (`random_header`) before any other obfuscated payload. The
server MUST read exactly 64 bytes before any further parsing.

**Per-session derivation (informative summary; §4.2 is normative):**

- `read_key`  ← `SHA256( random_header[8..40]              || secret[0..16] )`  (32 bytes)
- `read_iv`   ← `random_header[40..56]`                                          (16 bytes)
- `write_key` ← `SHA256( reverse(random_header[24..56])    || secret[0..16] )`  (32 bytes)
- `write_iv`  ← `reverse(random_header[8..24])`                                  (16 bytes)

**Magic-tag admission.** After server-side decryption of
`random_header` with `(read_key, read_iv)`, the four bytes at decrypted
offset `[56..60)` MUST equal one of `0xdddddddd`, `0xeeeeeeee`, or
`0xefefefef` (little-endian uint32). Any other tag MUST trigger silent
close per §5.

**Top-level MUSTs (the four invariants Round-6 fixed):**

1. Implementations MUST NOT derive any key or IV material from
   wire-visible state other than the bytes of `random_header` named
   above. Peer IP, timestamp, frame count, and similar context MUST
   NOT influence derivation.
2. Implementations MUST maintain the AES-CTR counter continuously
   across all subsequent WebSocket frames in the session; per-frame
   counter reset is FORBIDDEN.
3. Implementations SHOULD zeroise `secret[0..16]` from working memory
   once `(read_key, write_key)` have been derived. `(read_key,
   read_iv, write_key, write_iv)` themselves remain in cipher state
   for the lifetime of the connection. (Promotion to MUST is deferred
   to v0.2.0 once reconnect-handshake lifecycle is pinned.)
4. Per-session `random_header` from a CSPRNG yields fresh
   `(read_key, read_iv, write_key, write_iv)` per session — AES-CTR
   counter reuse across sessions sharing the same secret is therefore
   structurally impossible, provided invariant 1 holds.

The pedigree, byte-by-byte rationale for the slice indices, and the
audit trail to the v1 reference implementation are recorded in
[`ERRATA.md`](ERRATA.md) entry E-001 (KDF derivation rationale).

### 4.2 Normative derivation

Let `random_header` be the 64-byte client-emitted header read after
the §3 Session Header. Let `secret[0..16]` be the 16-byte key
extracted per `secret-format.md` §1. Let `reverse(x)` denote
byte-reversal of byte-slice `x`.

Implementations MUST compute:

```
read_key  = SHA256( random_header[8..40]              || secret[0..16] )    (32 bytes)
read_iv   = random_header[40..56]                                            (16 bytes)
write_key = SHA256( reverse(random_header[24..56])    || secret[0..16] )    (32 bytes)
write_iv  = reverse(random_header[8..24])                                    (16 bytes)
```

Implementations MUST initialise two AES-256-CTR streams with the
derived `(read_key, read_iv)` and `(write_key, write_iv)` and MUST
maintain each CTR counter continuously across all subsequent
WebSocket frames in the session; per-frame counter reset is
FORBIDDEN.

**Server-side validation.** The server MUST decrypt `random_header`
in place using `(read_key, read_iv)`, advancing the read counter by
64 bytes. The four bytes at decrypted offset `[56..60)` MUST equal
one of the three magic-tag values `0xdddddddd`, `0xeeeeeeee`, or
`0xefefefef` (little-endian uint32). Any other tag MUST trigger
silent close per §5.

Implementations MUST NOT derive any key or IV material from
wire-visible state other than the bytes of `random_header` named
above; in particular, peer IP, timestamp, or frame count MUST NOT
influence derivation. Implementations SHOULD zeroise `secret[0..16]`
from working memory once `(read_key, write_key)` have been derived;
`(read_key, read_iv, write_key, write_iv)` themselves remain in
cipher state for the lifetime of the connection.

### 4.3 Idle session (zero-length payload)

A Session Header followed by zero octets of obfuscated payload is
WELL-FORMED and represents an idle session. The server MUST NOT
treat this as `MALFORMED`. The CTR counter advances by zero bytes;
subsequent data (if sent) continues from the position established by
the `random_header` decryption. EOF after the 64-byte
`random_header` and before any further obfuscated bytes is
indistinguishable from a normally-terminated idle session and MUST
NOT be treated as an error by the §5 telemetry mapping.

## 5. Errors and silent close

Type3 defines a small, stable catalogue of error classes used by both
this specification's prose and the conformance vector envelope. The
classes map 1:1 to `t3_result_t` enumerants in `lib/include/t3.h` (drop
the `T3_ERR_` prefix):

| Class                  | `t3.h` enumerant            | Use case                                                                       |
|------------------------|-----------------------------|--------------------------------------------------------------------------------|
| `MALFORMED`            | `T3_ERR_MALFORMED`          | Header parse failure: bad command type, non-zero flags at v0.1.0, truncated bytes, unmasked client frame, Text-opcode frame, etc. |
| `UNSUPPORTED_VERSION`  | `T3_ERR_UNSUPPORTED_VERSION`| `version` byte greater than the receiver's maximum supported version (§6).      |
| `INVALID_ARG`          | `T3_ERR_INVALID_ARG`        | Caller-side API misuse (e.g. `NULL` output pointer); never observed on the wire. |
| `INTERNAL`             | `T3_ERR_INTERNAL`           | Programmer assertion failures; MUST NOT appear on conformance wire vectors.    |

Adding a new class is a breaking change requiring a single coordinated
PR touching `lib/include/t3.h`, `secret-format.md`, and this file (per
style-guide §3).

**Silent close (definition).** A *silent close* is the server's
exclusive on-wire response to any Type3 error class: no response
bytes are emitted to the Type3 stream. There is no Type3
protocol-level error frame, status code, or reason string. The
server MUST NOT emit a WebSocket Close frame with any Type3-specific
payload or close code; it MUST terminate the underlying TCP/TLS
connection directly, making the close indistinguishable at the
network layer from an idle connection drop.

The term "silent close" elsewhere in this document (§2 framing
violations, §3 header validation, §4 magic-tag mismatch, §6 version
mismatch) refers to this same on-wire contract — every error class
maps onto an identical observable: zero response bytes followed by
transport teardown.

**Timing.** The delay between error detection and transport teardown
is governed by [`anti-probe.md`](anti-probe.md) §7 (deliverable of
story 1.3): a uniform-random delay in `[50, 200]` ms, sampled from
the host's cryptographic RNG. Pending publication of story 1.3,
implementations MUST schedule silent close after a uniform-random
delay drawn from `[50, 200]` ms (sampled via
`t3_silent_close_delay_sample_ns` per `lib-v0.1.0`); this is a
forward citation and the canonical timing contract lives in
`anti-probe.md` once 1.3 is published. This document does NOT
redefine the timing envelope.

**Telemetry separation.** The error class is recorded in the server's
local stats counters (`bad_header_drops`, etc.) for operational
visibility but is NEVER signalled on the wire. A peer observing only
the network has no way to distinguish a `MALFORMED` close from an
`UNSUPPORTED_VERSION` close from a connection drop caused by routing
loss (anti-probe invariant AR-C2).

## 6. Version Negotiation

Type3 negotiates protocol version in-band via the 4-byte Session
Header's `version` octet (§3). There is no out-of-band version
discovery channel — no DNS TXT record, no HTTP probe endpoint, no
sideband server-info exchange. The rationale for excluding out-of-band
discovery is recorded in §7 Contested Decisions.

**Server-side dispatch.** A server with maximum supported version
`server_max_version`, observing an incoming Session Header carrying
`peer_version`, dispatches as follows:

- `peer_version <= server_max_version` and otherwise well-formed: the
  server processes the connection at `peer_version` semantics.
- `peer_version > server_max_version`: the server records error class
  `UNSUPPORTED_VERSION` for local telemetry (§5) and silent-closes
  per §5. No version-hint, no `server_max_version` value, and no
  retry-elsewhere advice are emitted on the wire (anti-probe invariant
  AR-C2).

**Client-side dispatch.** A client whose required minimum version
`client_min_version` exceeds the server's silently-observed capability
(detected via repeated silent close at the client's preferred version)
falls back per the FR43 retry-tier contract specified in
[`anti-probe.md`](anti-probe.md) §3 (deliverable of story 1.3). On
fallback, the client re-dials with `version = client_min_version` per
FR43; if even the minimum fails, the client surfaces a host-stack-
neutral failure (UX-DR catalogue, story 1.4) without leaking which
version was attempted.

**Flags vs version axis (FR2).** The §3 `flags` field extends the
protocol within a fixed `version`; only structural / parser-dispatch
changes warrant a `version` bump. A future version that adds a new
binary feature gated by a flag bit MAY ship as `version = 0x01` with a
new flag, provided existing v1 parsers can ignore the flag without
miscomputing payload boundaries. Conversely, any change that breaks
binary compatibility for a v1-conforming parser MUST bump `version`.
This separation keeps the unchanged-command-type promise (FR2): a v2
endpoint that has not changed `0x01 MTPROTO_PASSTHROUGH` semantics is
still conformant when speaking to a v1 peer.

## 7. Contested Decisions

This section records design alternatives that were considered and
rejected during the v0.1.0-draft authoring cycle, together with the
binding reason. These are not open questions — the rejection is
normative for the v0.1.x series. See workspace-root
`docs/contested-decisions.md` for the cross-cutting aggregate index.

**Out-of-band version negotiation (DNS TXT / HTTP probe endpoint).**
Rejected. An out-of-band channel — for example a TXT record at
`_t3._tcp.<host>` advertising supported versions, or an HTTP `GET
/_t3/versions` endpoint — would leak the relationship between the
hostname and the Type3 protocol fingerprint, contradicting the AR-C1
vocabulary firewall and exposing hosts to passive enumeration by a
DPI/TSPU adversary. In-band negotiation via the §3 `version` octet
keeps the relationship invisible to an unauthenticated observer; the
cost (one extra silent-close round trip on version skew) is bounded
and is paid only by clients with mismatched assumptions.

**8-byte header with magic prefix (e.g. `T3\x00\x00` followed by a
4-byte body).** Rejected. The 4-byte header ceiling matches existing
MTProto framing efficiency; a magic prefix is itself a fingerprint
that adversaries can match against without decryption, defeating the
point of an obfuscated transport. AR-S1 fixes the budget at 4 bytes
on the wire.

**Encode version inside the `flags` field (e.g. low 4 bits as
version).** Rejected. The §3 axes are deliberately separate: `version`
is structural (parser-dispatch granularity), `flags` is feature-bit
(within-version extension granularity). Conflating them forces every
future feature bit to recompile the parser-dispatch table and removes
the FR2 unchanged-command-type guarantee. Future versions therefore
keep the byte budget split as `1 byte version | 2 bytes flags`.

## 8. FR Coverage

This table maps each functional requirement intersected by the wire
format to the §-paragraph(s) where the normative content lives.
Authoritative FR text lives in
`_bmad-output/planning-artifacts/prd.md`; this table is a navigation
index, not a redefinition.

| FR    | Requirement summary                                                                  | Wire-format location(s)                                                       |
|-------|---------------------------------------------------------------------------------------|--------------------------------------------------------------------------------|
| FR1   | MTProto traffic carried over WebSocket inside TLS.                                    | §1 (TLS + WS upgrade), §2 (Binary opcode + fragmentation), §3 cmd `0x01 MTPROTO_PASSTHROUGH`, §4 (obfuscated-2 stream layered on the WS payload). |
| FR2   | Extensibility via flags without breaking unchanged command types.                     | §3 `flags` field (u16 LE), §6 (version vs flags axis separation).             |
| FR3   | Reserved command-type slots for forward growth.                                       | §3 registry table — `0x02 HTTP_DECOY_MIMIC` reserved-not-allocated; `0x03–0xFE` reserved for future allocation. |
| FR4   | In-band version negotiation; silent close on mismatch.                                | §3 `version` field, §5 silent close, §6 negotiation rules.                    |
| FR40  | Canonical wire-protocol spec exists and covers Session Header + command-type namespace + anti-probe semantics. | This entire document; specifically §3 (header + namespace), §5 (silent close + class catalogue), §6 (version), with anti-probe timing delegated to `anti-probe.md` (story 1.3). |
