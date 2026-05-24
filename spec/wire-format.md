---
spec_version: 0.1.0-draft
last_updated: 2026-05-23
status: draft
amendment_log:
  - id: W-001
    date: 2026-04-27
    summary: §4 obfuscated stream lifted from ERRATA-deferred to normative — AES-256-CTR / KDF formulae inline, magic-tag admission gate, MUST-NOT-derive-from-IP, MUST-CTR-continuous, SHOULD-zeroise-secret
  - id: W-002
    date: 2026-04-27
    summary: Round-8 close-out — D2 flat MALFORMED, D3 (cmd,version) dispatch table, D4 subprotocol MUST-NOT-echo, D6 HTTP/1.1 hard-pin, GET-target structured (host,path) rule, EOF-during-fragmentation contract, idle-session accept
  - id: W-003
    date: 2026-05-06
    summary: Epic 1a — allocated command_type=0x04 (T3_CMD_BENCH); split 0x03–0xFE reserved row; added §3.1 BENCH command doc; lib-v0.1.1 ABI patch bump
  - id: W-004
    date: 2026-05-21
    summary: Story 11-1 — allocated flags bit 0 as T3_FLAG_PADDING with negotiation semantics; defined §2.1 padding frame convention (first decrypted byte 0xFE); Invariant 2 parenthetical padding note in §4.1; version fallback note in §6; two contested decisions in §7; FR47 and FR48 coverage rows in §8.
  - id: W-005
    date: 2026-05-21
    summary: "Editorial §2.1 clarifications (deferred from 11-1 review): (a) zero-payload WS frame cannot be a padding frame — explicit MALFORMED note added; (b) padding detection ordering constraint — only applicable after §4 KDF derivation completes."
  - id: W-006
    date: 2026-05-23
    summary: Story 12-2 — integrated HTTP stream transport mode; defined GET/POST auto-detection dispatch; updated connection establishment (§1) and chunked framing rules (§2.2); clarified transport independence of padding (§2.1) and obfuscation/KDF (§4); clarified silent close in HTTP stream mode (§5); added contested decision entry (§7) and FR49/FR50 coverage rows (§8).
---

> **Normative.** This document defines required behaviour for conforming
> Type3 implementations. Code in `lib/` is illustrative; where this
> document and `lib/` differ, this document wins.

# Wire Format

The end-to-end shape of a Type3 connection: TLS → WebSocket upgrade OR HTTP
stream → Session Header → obfuscated-2 MTProto. This is the document
implementers live in.

## 1. Connection establishment

A Type3 connection is layered as TLS → WebSocket upgrade OR HTTP stream → Session
Header → obfuscated-2 MTProto. This section is normative for the lower
two layers (TLS + transport framing); §3 defines the Session Header that
follows.

**TLS layer.** TLS terminates externally to the Type3 protocol. This
specification is host-stack-neutral (NFR-S): it does not name a TLS
implementation, and conforming endpoints MAY use any TLS 1.2 or TLS
1.3 stack provided by their host environment. The bytes carried inside
the TLS `application_data` record are either a standards-compliant WebSocket
stream per RFC 6455 or a chunked HTTP stream.

### 1.1 Transport mode dispatch

Upon receiving a client connection, the server MUST inspect the first HTTP request line and HTTP headers to auto-detect the transport mode. This auto-detection operates as follows, in order:

1. **WebSocket Mode**: If the first request line begins with `GET ` **and** the HTTP headers contain `Upgrade: websocket` (case-insensitive value match), the server MUST treat the connection as WebSocket mode (WS) and proceed according to §1.2.
2. **HTTP Stream Mode**: If the first request line begins with `POST ` **and** the HTTP headers contain `Transfer-Encoding: chunked` (case-insensitive), the server MUST treat the connection as HTTP stream mode and proceed according to §1.3.
3. **`GET` without `Upgrade: websocket`**: If the first request line begins with `GET ` but the headers do not contain `Upgrade: websocket`, the connection is `MALFORMED` per §5 and the server MUST trigger a silent close. This prevents a non-upgrade GET from silently entering an undefined state.
4. **`POST` without `Transfer-Encoding: chunked`**: If the first request line begins with `POST ` but the headers do not contain `Transfer-Encoding: chunked` (e.g. the client sent `Content-Length` instead), the connection is `MALFORMED` per §5 and the server MUST trigger a silent close. Type3 HTTP stream mode requires chunked encoding in both directions.
5. **Other HTTP Methods**: If the request uses any other HTTP method (e.g. `PUT`, `DELETE`, `HEAD`), or does not conform to a valid HTTP/1.1 request line, the connection is `MALFORMED` per §5 and the server MUST trigger a silent close.

There is no out-of-band signaling or TLS extension used for transport mode negotiation. The client's preferred transport mode is derived from the secret configuration parameters (`secret-format.md` §2.4), and the server auto-detects this choice by observing the request line and headers.

### 1.2 WebSocket mode

When the transport mode is dispatched as WebSocket mode (WS), the client and server MUST negotiate a WebSocket connection.

The client initiates a single HTTP/1.1 `GET` request. v0.1.0 hard-pins HTTP/1.1 as the carrier; HTTP/2 extended CONNECT (RFC 8441) and HTTP/3 extended CONNECT (RFC 9220) are RESERVED for v0.2 and MUST NOT be used by a v0.1.0 conforming endpoint.

The HTTP/1.1 GET request target is constructed from the parsed secret (`secret-format.md` §1.1) as:

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

### 1.3 HTTP stream mode

When the transport mode is dispatched as HTTP stream mode, the client and server exchange data over a full-duplex HTTP/1.1 connection using chunked transfer encoding in both directions.

The client initiates the streaming session by sending a `POST` request. The client-emitted HTTP headers and request line MUST conform to the following template:

```http
POST /<path> HTTP/1.1\r\n
Host: <host>\r\n
Content-Type: application/octet-stream\r\n
Transfer-Encoding: chunked\r\n
\r\n
```

Where `<host> = result.host` and `<path> = result.path` are derived from the structured `(host, path)` pair in the parsed secret. The path construction rule from §1.2 applies identically (i.e. if `result.path` is empty, the target path is `/`). The client MUST NOT include a `Content-Length` header. The request target `/<path>` MUST NOT include query parameters — in particular, the `?t=<mode>` transport parameter is extracted from the secret by the client (per `secret-format.md` §2.4 / §5.2) and MUST NOT be forwarded to the server in the POST request-target URI.

**Server-side path matching (NORMATIVE).** The server MUST match the POST request target against `result.path` using the path portion only, ignoring any query string that the client may (erroneously) append. If the server operates behind a reverse proxy (§1.4), the proxy configuration MUST be set to forward the path without query-string rewriting. The query parameter `?t=<mode>` is a client-local instruction carried in the secret; it has no meaning at the HTTP transport layer and MUST NOT influence server-side routing decisions.

The client starts streaming the payload immediately after the trailing `\r\n` of the HTTP header block. The first 4 bytes of the client's HTTP chunked payload stream form the Session Header per §3, followed by the 64-byte `random_header` and the obfuscated stream per §4.

The server MUST respond with the following HTTP/1.1 response:

```http
HTTP/1.1 200 OK\r\n
Content-Type: application/octet-stream\r\n
Transfer-Encoding: chunked\r\n
\r\n
```

The server starts streaming its payload (the server's obfuscated-2 stream) inside HTTP chunks immediately after the trailing `\r\n` of the response header block. The server's response stream does NOT contain a Session Header (matching the WebSocket response semantics).

Both client and server MUST write outgoing bytes using the chunked transfer coding format described in §2.2.

### 1.4 Frontend requirements (HTTP stream)

This section is informative (non-normative). 

The Type3 server is designed to co-exist with a standard web server on port 443 by residing behind an L7 reverse proxy (deployment example: `nginx`) that terminates TLS.

**WebSocket and HTTP Stream Co-existence**: A single proxy location block can forward both WebSocket and HTTP stream modes to the same upstream Type3 endpoint. While WebSocket mode requires `Upgrade` and `Connection` headers to switch protocols, HTTP stream mode does NOT require them. 

**Nginx Configuration for HTTP Stream Mode**: To ensure full-duplex, low-latency streaming in HTTP stream mode, the reverse proxy MUST NOT buffer requests or responses. For `nginx`, the following directives are required in the routing context:

*   `proxy_request_buffering off;` — disables buffering of client POST bodies, passing bytes upstream immediately in real-time.
*   `proxy_buffering off;` — disables buffering of upstream responses, passing bytes to the client immediately.
*   `client_max_body_size 0;` — permits infinite chunked POST requests for long-lived streaming connections.
*   `proxy_read_timeout 86400s;` and `proxy_send_timeout 86400s;` — **REQUIRED** for long-lived HTTP stream sessions. Without these, nginx will close idle connections after its default 60-second timeout, terminating the transport session mid-stream. Set to at least the expected maximum session lifetime.

This deployment model is documented in <!-- ban-list-allow:doc-reference --> `docs/mtproxy3-architecture.md` <!-- /ban-list-allow -->. Direct TLS termination by the Type3 server itself (without a frontend reverse proxy) is equally conforming (FR1, NFR-S).

See diagram [`diagrams/connection-flow.mmd`](diagrams/connection-flow.mmd).

## 2. WebSocket and HTTP chunked framing

After connection establishment (§1), endpoints exchange data framed either as WebSocket frames (WebSocket mode, §1.2) or HTTP chunks (HTTP stream mode, §1.3).

**WebSocket framing rules.** In WebSocket mode, endpoints exchange WebSocket frames per RFC 6455. Type3 imposes the following constraints on top of RFC 6455:

*   **Error class for WebSocket violations.** A frame violating any WebSocket constraint below is `MALFORMED` per §5 and MUST trigger a silent close. v0.1.0 distinguishes only two wire-observable error classes (`MALFORMED` vs `UNSUPPORTED_VERSION`); finer sub-classification of WebSocket framing violations is not normative. Implementations MAY log the specific violation locally for operational visibility (e.g. `bad_mask`, `bad_opcode`, `rsv_set`), but the on-wire response is identical for every flavour.
*   **Masking.** Per RFC 6455 §5.3, client→server frames MUST set MASK=1 and carry a 4-byte masking key; server→client frames MUST set MASK=0. A frame that violates either rule is `MALFORMED`.
*   **Opcode.** Type3 payload frames MUST use opcode `0x2` (Binary). When a Binary message is fragmented per RFC 6455 §5.4, the initial frame carries opcode `0x2` and subsequent continuation frames carry opcode `0x0` (Continuation); the parser MUST accept both. Frames with opcode `0x1` (Text) are `MALFORMED`. Frames with reserved or unrecognised opcodes (values other than `0x0`–`0x2`, `0x8`–`0xA`) are `MALFORMED`. Control frames (`0x8` Close, `0x9` Ping, `0xA` Pong) are passed to the WebSocket layer below; the Type3 layer ignores them apart from observing a Close, after which the underlying transport is torn down without further Type3-level processing. Control frames MAY be interleaved during Session Header reassembly; the receiver MUST NOT discard accumulated header bytes upon receiving a control frame.
*   **Fragmentation.** A logical Type3 record (the 4-byte Session Header, or a window of obfuscated-2 stream bytes) MAY span two or more WebSocket frames; the parser MUST buffer until enough payload bytes have been accumulated and then advance. In particular, a Session Header MAY be delivered as `[2 bytes][2 bytes]`, `[1][1][1][1]`, or any other partition summing to four bytes; the parser MUST NOT assume frame-boundary alignment with logical record boundaries (FR1, FR2). A single WebSocket frame MAY also carry the 4-byte Session Header followed immediately by an arbitrary prefix of the obfuscated-2 stream; parsers MUST consume exactly four bytes of header and then route the remainder to the §4 stream consumer.
*   **EOF during Session Header reassembly.** If the underlying transport closes (TCP FIN / TLS close_notify / WebSocket Close) before the parser has accumulated four bytes of Session Header, the partial header is `MALFORMED`. A receiver that has buffered zero header bytes when EOF arrives MUST also treat the connection as `MALFORMED` for local telemetry (no header was ever supplied); the §5 silent-close contract still governs the wire response (i.e., no response bytes are emitted regardless).
*   **Other RFC 6455 constraints unchanged.** RSV bits MUST be zero; an unmasked client frame, a masked server frame, or any reserved-bit non-zero frame is `MALFORMED`.

### 2.1 Padding frames

**Normative definition.**
- A WebSocket Binary frame or an HTTP chunk whose first decrypted byte (post-AES-CTR decryption) equals `0xFE` is a **padding frame**.
- The receiver MUST discard the frame/chunk payload (do not relay it to the MTProto layer).
- The receiver MUST advance the AES-CTR counter by the full payload length of the frame/chunk, exactly as with any other payload frame/chunk (Invariant 2 applies).
- The padding frame/chunk payload bytes following the `0xFE` marker MUST be indistinguishable from random bytes to a passive observer (e.g., cryptographically random fill or AES-CTR encryption of zeroes are both conforming).
- The minimum padding payload length is 1 byte (consisting of the `0xFE` marker alone). The maximum length is 65535 bytes (the WebSocket frame payload limit, though in practice implementations SHOULD limit padding payloads to 512 bytes for MTU friendliness).

**Transport Independence.** Padding frame detection operates identically whether the enclosing frame is a WebSocket Binary frame or an HTTP chunk. The `0xFE` marker resides inside the AES-CTR encrypted payload; the transport framing layer (WebSocket or HTTP chunked) is transparent to padding semantics. The receiver MUST discard the padding payload, advance the AES-CTR counter by the frame/chunk length, and proceed to the next frame/chunk in either transport mode.

**Zero-payload frames.** A Binary WebSocket frame with a payload length of zero carries no bytes and therefore cannot have a first decrypted byte. Such a frame MUST NOT be classified as a padding frame. It is governed exclusively by the WebSocket framing rules: if it arrives with a valid opcode (`0x2` or `0x0`) and otherwise conforming WS header, the receiver MUST treat it as a zero-length data frame and advance no CTR bytes. No special padding-detection logic applies. In HTTP stream mode, a zero-size chunk represents the terminal chunk (§2.2) and signifies the end of the stream rather than a zero-payload data chunk.

**Ordering constraint.** Padding frame detection (i.e., decrypting the first payload byte and comparing it to `0xFE`) is only applicable after the §4 AES-CTR setup is complete. The §4 KDF derivation requires the 64-byte `random_header` to be fully received and decrypted first. A WebSocket Binary frame or HTTP chunk that arrives before the 64-byte `random_header` exchange has completed MUST be treated as `MALFORMED` (premature payload before handshake completion); padding detection MUST NOT be attempted on such frames/chunks.

**`0xFE` Namespace Distinction.** The `0xFE` byte has two entirely different contexts:
- **Session Header** (§3): `0xFE` is in the reserved `0x05–0xFE` range of `command_type` where the receiver duty is `MALFORMED` (the header is plaintext).
- **Obfuscated payload** (§2.1): the first decrypted byte `0xFE` of a decrypted payload frame/chunk marks a padding frame.

These are distinct namespaces: the session header is plaintext, whereas padding detection happens on decrypted payload bytes after AES-CTR.

**CTR Continuity.** Invariant 2 (§4.1) applies to padding frames/chunks without exception. The AES-CTR counter MUST advance continuously through padding frames/chunks — padding bytes participate in the CTR stream exactly like payload bytes. A desynchronized counter (e.g., from a receiver that skips CTR advancement on padding frames/chunks) will corrupt all subsequent bytes.

### 2.2 HTTP chunked framing

When the connection is in HTTP stream mode, data is framed using standard HTTP chunked transfer coding per RFC 9112 §7.1.

**Chunk Format**: Each chunk is formatted as:
```
<chunk-size> [; chunk-ext] \r\n
<chunk-data> \r\n
```
Where `<chunk-size>` is a hexadecimal number indicating the number of octets in `<chunk-data>`. The maximum permitted `<chunk-size>` for a Type3 HTTP stream chunk is **65535 bytes** (0xFFFF); a receiver that encounters a chunk-size value exceeding this limit MUST treat the connection as `MALFORMED` and trigger a silent close. Conforming Type3 implementations MUST NOT emit chunk extensions; receivers MUST parse and skip chunk extensions up to a maximum of **255 bytes** (excluding the trailing `\r\n`). A chunk extension line exceeding 255 bytes is `MALFORMED`.

**Stream Mapping**: The obfuscated-2 payload stream (or Session Header in the first chunk) is mapped directly into the `<chunk-data>` blocks. Chunk boundaries do NOT align with cipher block boundaries (AES-CTR block size of 16 bytes) or logical packet boundaries. The receiver MUST process incoming bytes in stream fashion, routing them through the AES-CTR decryptor as they arrive.

**Fragmentation**: A logical Type3 record (the 4-byte Session Header, or a segment of the obfuscated-2 stream) MAY span multiple HTTP chunks. The parser MUST buffer incoming data until the required number of bytes has been accumulated. Specifically:
- The 4-byte Session Header inside the client's POST body MAY be split across multiple chunks (e.g. `[2 bytes]` in chunk 1, `[2 bytes]` in chunk 2). The parser MUST accumulate exactly 4 bytes before executing command/version validation.
- The 64-byte `random_header` (§4.2) MAY be split across chunks. The key-derivation engine MUST buffer until all 64 bytes are accumulated before initializing the AES-CTR state.

**EOF and Connection Termination**:
- **Normal Close**: Under normal operation, a stream direction is terminated by sending a terminal chunk (`0\r\n\r\n`).
- **EOF during Session Header**: If the HTTP chunked stream terminates (closes TCP/TLS or receives the terminal chunk `0\r\n\r\n`) before the 4-byte Session Header is fully accumulated, the connection is `MALFORMED` per §5 and the server MUST trigger a silent close.
- **Empty payload**: An HTTP chunk with a chunk-size of zero (`0\r\n\r\n`) is the terminal chunk and signifies the end of the stream direction. Any other parsing state where a non-zero `<chunk-size>` is followed immediately by `\r\n\r\n` without the declared number of data bytes is `MALFORMED`.

**Error class for §2.2 violations**: A frame violating any §2.2 chunked framing rules (e.g., invalid hexadecimal length, incorrect `\r\n` delimiters, premature stream termination) is `MALFORMED` and MUST trigger a silent close.

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

**Command-type registry (v0.1.1).** Each row gives a sender duty (a
producer MUST NOT emit this byte) and a receiver duty (a v0.1.1 parser
MUST react this way on receipt). The two duties are stated separately
so future versions can promote a reserved slot without renumbering:

| Value       | Name                  | Sender duty (v0.1.1)                       | Receiver duty (v0.1.1)                                                          |
|-------------|-----------------------|--------------------------------------------|----------------------------------------------------------------------------------|
| `0x00`      | reserved              | MUST NOT emit                              | `MALFORMED`. Reserved as a sentinel for "no command type set"; symmetric with `0xFF`. |
| `0x01`      | `MTPROTO_PASSTHROUGH` | MUST emit for every production conn (FR1)  | Process the connection at v0.1.x semantics (single production value).           |
| `0x02`      | `HTTP_DECOY_MIMIC`    | MUST NOT emit at v0.1.x                    | `MALFORMED` (reserved-not-allocated, FR3).                                       |
| `0x03`      | reserved              | MUST NOT emit at v0.1.x                    | `MALFORMED` (reserved for future allocation, FR3).                              |
| `0x04`      | `T3_CMD_BENCH`        | MUST NOT emit in production traffic        | Parse succeeds (`T3_OK`) at v0.1.1; server MUST gate dispatch by build flag + runtime flag. See §3.1. |
| `0x05–0xFE` | reserved              | MUST NOT emit at v0.1.x                    | `MALFORMED` (reserved for future allocation, FR3).                              |
| `0xFF`      | reserved              | MUST NOT emit                              | `MALFORMED`. Reserved as the upper boundary of the command-type space, symmetric with `0x00` (cf. `secret-format.md` §1, where `0xFF` marks the *secret* version sentinel — distinct field, distinct namespace). |

**Version field.** At v0.1.0 the `version` byte is `0x01`. A header
carrying `version > 0x01` triggers §6 Version Negotiation. A header
carrying `version == 0x00` is `MALFORMED`.

**Flags field.** `flags` is a 16-bit value transmitted in little-endian
byte order (low byte first). At v0.1.x every bit MUST be zero; a
non-zero `flags` value is `MALFORMED`. Future versions MAY allocate
flag bits without bumping the transport ID (FR2): the `version` field
governs structural / parser-dispatch changes; `flags` governs
within-version feature extensions. The two axes are deliberately
separated (see §7).

| Bit  | Name              | v0.1.x    | v0.2.0                                    |
|------|-------------------|-----------|--------------------------------------------|
| 0    | T3_FLAG_PADDING   | MUST be 0 | Negotiates padding frame support (§2.1)   |
| 1    | reserved          | MUST be 0 | Reserved for T3_FLAG_TRAFFIC_PROFILE      |
| 2–15 | reserved          | MUST be 0 | Reserved for future allocation            |

A v0.2.0 client MAY set `T3_FLAG_PADDING` to signal that it supports padding frames (§2.1). A server supporting the flag MUST enable padding injection for this session. A v0.1.x server treats any non-zero `flags` as `MALFORMED` (existing §3 rule, unchanged).

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
| `0x01, 0x04` (known at v0.1.1) | `> 0x01` (unknown) | `UNSUPPORTED_VERSION`   | Known command at unknown version — version negotiation per §6.            |
| `0x02, 0x03, 0x05–0xFE` (unallocated at v0.1.1) | `0x01` (known) | `MALFORMED` | Unknown command at known version — reserved slot, no future semantics.    |
| `0x02, 0x03, 0x05–0xFE` (unallocated at v0.1.1) | `> 0x01` (unknown) | `UNSUPPORTED_VERSION` | Future-recognised: unknown command at unknown version is forward-compat — defer to version negotiation. |
| `0x02, 0x03, 0x05–0xFE` (unallocated at v0.1.1) | `0x00` (sentinel) | `MALFORMED` | Sentinel version fires before cmd classification — MALFORMED regardless.  |
| `0x00` / `0xFF` (sentinel) | (any)        | `MALFORMED`             | Sentinel slots are MALFORMED regardless of version.                        |
| `0x01, 0x04` (known at v0.1.1) | `0x00` (sentinel) | `MALFORMED`       | Sentinel version slot is MALFORMED regardless of command type.            |

**Concrete vectors.** The following hex sequences illustrate each
disposition; matching machine-verifiable vectors live in
`conformance/vectors/unit.json` under section key `session-header`:

- Valid v1, no flags: `01 01 00 00` — `command_type = MTPROTO_PASSTHROUGH`, `version = 1`, `flags = 0`.
- `01 01 01 00` — `command_type = MTPROTO_PASSTHROUGH`, `version = 1`, `flags = T3_FLAG_PADDING`. Disposition is version-dependent: `MALFORMED` at v0.1.x (non-zero flags); valid at v0.2.0 (flag bit 0 allocated as `T3_FLAG_PADDING`).
- `MALFORMED` (unknown command type at v0.1.0): `02 01 00 00` — `0x02` is reserved-not-allocated.
- Forward version (triggers §6): `01 02 00 00` — a v0.1.0 server returns `UNSUPPORTED_VERSION` for telemetry and silent-closes.

See diagram [`diagrams/session-header.mmd`](diagrams/session-header.mmd).

### 3.1 BENCH command (`0x04`) — experimental, dev-only <a id="W-003"></a>

`command_type=0x04` (`T3_CMD_BENCH`) is allocated by Epic 1a (lib-v0.1.1) as a
dev-only bench-echo command. It is intentionally NOT a production feature; server
dispatch MUST be gated by a build-time flag **and** a runtime flag (default OFF in
production builds). This section is normative for lib-v0.1.1.

**Payload layout.** After the 4-byte Session Header, the first byte of the
obfuscated payload MUST be a sub-mode octet:

| Sub-mode | Value  | Semantics                                                           |
|----------|--------|---------------------------------------------------------------------|
| sink     | `0x01` | Server reads payload and discards; no data is echoed back.          |
| echo     | `0x02` | Server echoes every received byte back to the client unchanged.     |
| source   | `0x03` | Server generates and sends a payload of the negotiated size; client reads. |

Sub-modes `0x00` and `0x04–0xFF` are reserved. A server that receives an
unrecognised sub-mode MUST silent-close per §5.

**Server gate (mandatory).** A server MUST NOT dispatch `T3_CMD_BENCH` unless
**both** of the following are true:
1. The build was compiled with the bench-server build flag (`TELEPROTO3_BENCH=1`
   — defined by Story 1a-2).
2. The runtime flag `--bench-enable` (or equivalent) is active at startup.

A v0.1.1 server receiving `0x04` while either gate is OFF MUST silent-close per §5
exactly as if an unknown command type were received — no acknowledgement, no error
frame.

**Version compatibility.**
- v0.1.0 clients MUST NOT emit `command_type=0x04`; they have no `T3_CMD_BENCH`
  constant and the reserved slot was `MALFORMED` at v0.1.0.
- v0.1.1 clients MAY emit `0x04` only against a server known (out-of-band) to be
  compiled with the bench gate.
- A v0.1.0 server receiving `0x04 0x01 0x00 0x00` returns `MALFORMED` (former
  reserved-slot rule) — fully backward-compatible with the v0.1.1 library sending
  such a header only in bench context.

**Non-production invariant.** `T3_CMD_BENCH` `SHALL NOT appear in production
traffic` (quoting the `t3.h` constant comment). Any network capture of `0x04` on
a production endpoint is evidence of misconfiguration.

## 4. Obfuscated stream (AES-256-CTR setup) <a id="W-001"></a>

The bytes following the 4-byte Session Header form the obfuscated-2
MTProto stream. Header bytes (offsets 0–3 of the transport payload
stream) are NOT encrypted. The obfuscated-2 stream begins at byte 4
of that stream. WebSocket frame and HTTP chunk boundaries are independent of cipher
block boundaries and of MTProto frame boundaries: a parser MUST NOT
realign, re-segment, or pad the obfuscated-2 stream to fit WebSocket
frame or HTTP chunk boundaries; the stream is delivered to the cipher
byte-for-byte in arrival order across however many WebSocket frames
or HTTP chunks the underlying transport chose to emit (FR1).

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
   across all subsequent WebSocket frames or HTTP chunks in the session; per-frame
   or per-chunk counter reset is FORBIDDEN. (This invariant covers padding frames/chunks
   introduced in v0.2.0 — see §2.1.)
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
WebSocket frames or HTTP chunks in the session; per-frame or per-chunk counter reset is
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
payload or close code, and in HTTP stream mode, the server MUST NOT
send any HTTP error response status or body. Instead, it MUST terminate
the underlying transport directly (in HTTP stream mode, this is achieved
by emitting the terminal chunk `0\r\n\r\n` followed by immediate teardown
of the TCP/TLS connection), making the close indistinguishable at the
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

**Padding flag fallback (concrete example).** A v0.2.0 client setting `T3_FLAG_PADDING` against a v0.1.x server will observe a silent close (the server treats `flags != 0` as `MALFORMED`). The client MUST retry with `flags = 0` per the FR43 retry-tier contract before surfacing failure to the user.

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

**Padding as WS opcode or extension (e.g. `0x3` Text-as-padding or `Sec-WebSocket-Extensions: padding`).** Rejected. A separate opcode or WS extension for padding would be visible to any middlebox inspecting WS frame headers (which are NOT under AES-CTR). The Type3 padding convention places the `0xFE` marker inside the encrypted payload, making padding frames indistinguishable from data frames to any observer without the session key. This is the central anti-fingerprinting requirement.

**Padding via out-of-band signaling (HTTP header, TLS extension).** Rejected for the same reason as §7 existing entry on out-of-band version negotiation: leaks protocol semantics to passive observers.

**HTTP stream as separate document vs amendment.** Rejected. Establishing a separate specification document for HTTP stream mode would introduce significant redundancy, as HTTP stream mode and WebSocket mode share the identical Session Header (§3), key derivation and obfuscated payload structure (§4), error catalog and silent close timing (§5), version negotiation (§6), and anti-statistical obfuscation logic. Unifying both modes under a single wire-format document keeps the transport layers aligned, prevents document fragmentation, and simplifies compliance auditing.

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
| FR3   | Reserved command-type slots for forward growth.                                       | §3 registry table — `0x02 HTTP_DECOY_MIMIC` reserved-not-allocated; `0x04 T3_CMD_BENCH` (experimental, W-003); `0x03, 0x05–0xFE` reserved for future allocation. |
| FR4   | In-band version negotiation; silent close on mismatch.                                | §3 `version` field, §5 silent close, §6 negotiation rules.                    |
| FR40  | Canonical wire-protocol spec exists and covers Session Header + command-type namespace + anti-probe semantics. | This entire document; specifically §3 (header + namespace), §5 (silent close + class catalogue), §6 (version), with anti-probe timing delegated to `anti-probe.md` (story 1.3). |
| FR47  | Statistical indistinguishability of Type3 traffic under passive DPI                  | §2.1 (padding frame convention), §3 (T3_FLAG_PADDING flag), §7 (contested: why inside AES-CTR) |
| FR48  | In-band padding negotiation via Session Header flags                                 | §3 (flag bit 0 allocation), §6 (fallback on v0.1.x rejection)                 |
| FR49  | HTTP stream transport mode                                                            | §1.3 (HTTP stream request/response), §2.2 (HTTP chunked framing details)      |
| FR50  | Server auto-detection of transport mode (GET vs POST dispatch)                         | §1.1 (Transport mode dispatch auto-detection rules)                          |
