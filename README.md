# teleproto3

**Type3** (`mtProxy3`) is a censorship-resistant transport for [Telegram Messenger](https://telegram.org): it tunnels MTProto over HTTPS via HTTP POST with chunked transfer encoding, to bypass DPI-based blocking (ТСПУ-resistant). WebSocket upgrade mode is supported for legacy clients but deprecated.

This repository contains:

| Directory | Contents |
|-----------|----------|
| `spec/` | Normative protocol specification |
| `lib/` | Reference C implementation (`libteleproto3 v0.6.0`) |
| `server/` | Server — fork of the official Telegram MTProxy with Type3 transport added |
| `conformance/` | Language-agnostic test harness and protocol vectors |

**Where `spec/` and `lib/` differ, `spec/` wins.**

---

## How it works

A Type3 connection uses HTTP stream mode as the primary transport:

**HTTP stream mode** (recommended, ТСПУ-resistant):
```
Client
  └── TLS 1.2/1.3
        └── HTTP/1.1 POST + Transfer-Encoding: chunked
              └── Session Header (4 bytes, little-endian)
                    └── obfuscated-2 MTProto (AES-256-CTR)
                          └── Telegram DC
```

**WebSocket mode** (legacy, deprecated):
```
Client
  └── TLS 1.2/1.3
        └── HTTP/1.1 WebSocket upgrade (GET /<path>, Host: <host>)
              └── Session Header (4 bytes, little-endian)
                    └── obfuscated-2 MTProto (AES-256-CTR)
                          └── Telegram DC
```

TLS terminates on an nginx (or equivalent) reverse proxy. The proxy forwards the connection to the Type3 server on a local port. The server auto-detects the transport mode by inspecting the HTTP request line (`POST` → HTTP stream, `GET` → WebSocket). The obfuscation stream sits **inside** the transport frames — transport headers are plaintext to nginx; only the payload is encrypted.

HTTP stream mode looks like a normal HTTPS POST to a REST API, making it resistant to WebSocket-specific DPI fingerprinting. New clients MUST use HTTP stream. See [`spec/wire-format.md` §1.3–§1.4](spec/wire-format.md) for the full specification and nginx configuration requirements.

---

## Secret format

A Type3 secret is the opaque blob clients receive (via QR code, deeplink, or paste):

```
offset  length  field
------  ------  ------------------------------------
0       1       marker = 0xff
1       16      key (raw bytes)
17      ≥ 1     domain = <host>[/<path>]  (UTF-8, no terminator)
```

Minimum total length: **18 bytes**. Domain field maximum: **512 bytes** (host + `/` + path combined).

The `host` and `path` sub-fields are split at the first `/` in the domain field. Both are pure ASCII; non-ASCII is rejected in `lib-v0.1.0`. The structured `(host, path)` pair — not the unsplit domain string — is used to construct the HTTP POST path and `Host:` header (or WebSocket `GET` target for legacy clients).

---

## `libteleproto3` — reference implementation

### Quick start

```c
#include "t3.h"

/* Parse a secret (e.g. from a config file or user input). */
t3_secret_t *sec = NULL;
t3_result_t rc = t3_secret_parse(buf, len, &sec);
if (rc != T3_OK) { fprintf(stderr, "%s\n", t3_strerror(rc)); return 1; }

/* Create a session and wire up your I/O callbacks. */
t3_session_t *sess = NULL;
t3_session_new(sec, &sess);
t3_secret_free(sec);   /* session copies what it needs; free sec now */

t3_callbacks_t cb = { .struct_size = sizeof cb,
                       .lower_send = my_send, .lower_recv = my_recv,
                       .frame_send = my_frame_send, .frame_recv = my_frame_recv,
                       .rng = my_rng, .monotonic_ns = my_clock };
t3_session_bind_callbacks(sess, &cb);

/* ... drive the session ... */

t3_session_free(sess);
```

### API surface (`include/t3.h`)

The public header is the **only stable ABI surface** of the library. Everything under `lib/src/` is private.

#### Secret

```c
t3_result_t t3_secret_parse(const uint8_t *buf, size_t len, t3_secret_t **out);
void        t3_secret_free(t3_secret_t *s);
t3_result_t t3_secret_serialise(const t3_secret_fields *in, uint8_t *out, size_t *inout_len);
t3_result_t t3_secret_validate_host(const char *host);
t3_result_t t3_secret_validate_path(const char *path);
void        t3_secret_zeroise(t3_secret_fields *fields);
```

#### Session

```c
t3_result_t t3_session_new(const t3_secret_t *s, t3_session_t **out);
void        t3_session_free(t3_session_t *sess);
t3_result_t t3_session_bind_callbacks(t3_session_t *sess, const t3_callbacks_t *cb);
```

#### Session header (wire-format §3)

```c
t3_result_t t3_header_parse(const uint8_t buf[4], t3_header_t *out);
t3_result_t t3_header_serialise(const t3_header_t *in, uint8_t buf[4]);
```

#### Version negotiation (wire-format §6)

```c
t3_result_t t3_session_negotiate_version(t3_session_t *sess,
                                         uint8_t peer_version,
                                         t3_version_action_t *out);
```

#### Anti-probe

```c
t3_result_t      t3_silent_close_delay_sample_ns(t3_session_t *sess, uint64_t *out_ns);
t3_result_t      t3_retry_record_close(t3_session_t *sess,
                                       uint64_t now_monotonic_ns,
                                       t3_retry_state_t *out_state);
t3_retry_state_t t3_retry_get_state(const t3_session_t *sess);
t3_result_t      t3_retry_user_retry(t3_session_t *sess);
```

#### Utility

```c
const char *t3_strerror(t3_result_t rc);
const char *t3_abi_version_string(void);
```

### Client-side transport API (`t3_client.h`)

Added in `v0.3.0`. Provides a complete client transport abstraction:
DNS resolve → TCP connect → TLS handshake → obfs2 init → AES-CTR encrypt/decrypt → HTTP-chunked framing.

All clients (tdlib, tdesktop, Telegram-Android, Telegram-iOS) use this API:

```c
#include "t3_client.h"

/* Create connection — endpoint_url is the HTTPS stream endpoint */
t3_client_stream *s = NULL;
t3_result_t rc = t3_client_create(
    "https://example.invalid:443/ws/abcdef",  /* HTTP stream endpoint */
    key_16bytes,   /* 16-byte proxy key from t3_secret_parse() */
    dc_id,         /* Telegram DC id (signed; negative = media) */
    &s
);
if (rc != T3_OK) { fprintf(stderr, "%s\n", t3_strerror(rc)); return 1; }

/* Register t3_client_get_fd(s) with poll/epoll; call pump() on events */
while (t3_client_get_state(s) != T3_CLIENT_STATE_READY)
    t3_client_pump(s);   /* drives TLS + obfs2 handshake */

/* Write MTProto plaintext → encrypted + chunked over TLS */
t3_client_write(s, mtproto_payload, payload_len);

/* Read decrypted MTProto payload from incoming data */
uint8_t buf[65536];
size_t n = 0;
t3_client_read(s, buf, sizeof(buf), &n);   /* one message per call */

t3_client_destroy(s);   /* close fd, free resources; NULL-safe */
```

See `lib/src/t3_client_stream.c`, `t3_client_ws.c`, `t3_http_stream.c` for the implementation.

### ABI stability

| Version | Rule |
|---------|------|
| `lib-v0.1.x` | ABI frozen. New enumerants in `t3_result_t` are permitted; consumers **MUST** treat unknown values as `T3_ERR_INTERNAL`. |
| `lib-v0.2.0` | Padding/splitting API, HTTP stream framing, transport mode enums (`T3_TRANSPORT_*`). |
| `lib-v0.3.0` | Client-side transport API (`t3_client.h`): `t3_client_create/pump/write/read/destroy`. |
| `lib-v0.4.0` | Optional client build (`T3_BUILD_CLIENT`), SOCKS5 auth hardening. |
| `lib-v0.5.0` | Client data-path correctness fixes, 8 MiB ring buffers, Android/Windows compat. |
| `lib-v0.6.0` | Windows/MSVC portability, prebuilt win+mac libs in GitHub releases. |

### Build

```bash
cd lib/build
make          # static library: libteleproto3.a
make test     # unit tests with ASan + UBSan
```

Requires: C11 compiler, POSIX make. No external dependencies. A Bazel `BUILD.bazel` is also provided.

---

## Specification (`spec/`)

Recommended reading order:

1. [`spec/glossary.md`](spec/glossary.md) — terms of art
2. [`spec/threat-model.md`](spec/threat-model.md) — adversary model and security properties
3. [`spec/non-goals.md`](spec/non-goals.md) — what Type3 does **not** defend against
4. [`spec/compliance-levels.md`](spec/compliance-levels.md) — Core / Full / Extended tiers
5. [`spec/secret-format.md`](spec/secret-format.md) — secret encoding
6. [`spec/wire-format.md`](spec/wire-format.md) — handshake, framing, version negotiation
7. [`spec/anti-probe.md`](spec/anti-probe.md) — silent-close rules, FR43 retry heuristic
8. [`spec/ux-conformance.md`](spec/ux-conformance.md) — client UI state machine
9. [`spec/conformance-procedure.md`](spec/conformance-procedure.md) — how to run the harness

The spec is licensed Apache 2.0 so that third-party implementations (including GPL-downstream code) can consume it without relicensing.

---

## Conformance harness (`conformance/`)

A language-agnostic harness that drives any Type3 implementation through the normative scenarios defined in `conformance/scenarios/`. Protocol vectors live in `conformance/baselines/`. See [`conformance/README.md`](conformance/README.md).

---

## Repository layout

```
teleproto3/
├── spec/           Normative protocol specification (Apache 2.0)
├── lib/            libteleproto3 — reference C implementation
│   ├── include/    Public headers: t3.h, t3_client.h, t3_features.h
│   ├── src/        Private implementation
│   │   ├── t3_client_stream.c   Client transport abstraction
│   │   ├── t3_client_ws.c       WebSocket frame write (masked) + read
│   │   ├── t3_http_stream.c     HTTP chunked stream framing
│   │   ├── t3_client_crypto.c   obfs2 KDF + AES-CTR
│   │   └── ...                  Core library sources
│   ├── tests/      Unit + integration tests
│   ├── build/      POSIX Makefile + Bazel BUILD
│   └── CHANGELOG.md
├── server/         MTProxy fork with Type3 transport
├── conformance/    Test harness + protocol vectors
├── CONTRIBUTING.md
├── SECURITY.md
└── OWNERSHIP.md
```

---

## Contributing

See [`CONTRIBUTING.md`](CONTRIBUTING.md). Key points:

- Changes to `spec/` change the protocol — use RFC 2119 keywords normatively (uppercase only).
- `lib/` must track `spec/`. When they differ, `spec/` wins.
- `server/` upstreams managed subdirectories (`common/`, `crypto/`, `jobs/`, `mtproto/`) via `git subtree` — avoid editing those outside a subtree pull.
- Every `lib/src/*.c` file must carry the reference-implementation banner. CI enforces this.

## Security

See [`SECURITY.md`](SECURITY.md) for the vulnerability disclosure policy.
