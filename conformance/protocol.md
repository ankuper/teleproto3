# Harness ↔ IUT protocol

> This document restates the stdin/stdout protocol normatively defined
> in [`../spec/conformance-procedure.md`](../spec/conformance-procedure.md).
> If they disagree, the spec wins.

Implementations-under-test (IUTs) do not need to be in C. The runner
invokes the IUT as a subprocess and speaks a simple line-delimited
JSON protocol.

## Framing

- One JSON object per line, UTF-8.
- Lines are terminated with `\n` (no CRLF).
- Both sides are strict: malformed lines close the conversation.

## Request → response shape

Runner to IUT:
```json
{"op": "<operation>", "args": { ... }}
```

IUT to runner:
```json
{"ok": true,  "result": { ... }}
{"ok": false, "error": "<machine-readable class>", "detail": "<human text>"}
```

## Operations

_TBD(conformance-v0.1.0):_ enumerate. Seed:

| op                     | args                     | result                     |
| ---------------------- | ------------------------ | -------------------------- |
| `parse_secret`         | `{"bytes": "<hex>"}`     | parsed secret struct       |
| `serialise_secret`     | `{"secret": {...}}`      | `{"bytes": "<hex>"}`       |
| `build_session_header` | `{"secret": {...}, ...}` | `{"bytes": "<hex>"}`       |
| `decode_frame`         | `{"bytes": "<hex>"}`     | decoded frame payload      |
| `handshake_trace`      | `{"scenario": "<id>"}`   | ordered trace of frames    |

Unknown `op` MUST produce `{"ok": false, "error": "UNSUPPORTED_OP"}`
(not crash).

## Error classes

- `MALFORMED` — input failed to parse.
- `UNSUPPORTED_VERSION` — spec version the IUT does not implement.
- `INTERNAL` — IUT bug (will be logged as a harness failure).
- `UNSUPPORTED_OP` — operation the IUT does not implement.

## Handshake

On startup the runner sends:
```json
{"op": "hello", "args": {"harness_version": "conformance-vX.Y.Z", "spec_version": "spec-vX.Y.Z"}}
```

IUT responds:
```json
{"ok": true, "result": {"iut_version": "...", "claims_level": "core|full|extended"}}
```

The runner logs the `iut_version` + `claims_level` in the report
header.
