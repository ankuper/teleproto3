# libteleproto3

Reference C implementation of the Type3 protocol. Consumed by the
`server/` fork and by the three client forks (tdesktop, iOS, Android).

> ⚠️ **`spec/` is the source of truth.** This library is illustrative.
> If implementation and spec disagree, the spec wins; file an issue at
> [`../.github/ISSUE_TEMPLATE/bug-lib.md`](../.github/ISSUE_TEMPLATE/bug-lib.md)
> or errata at [`../.github/ISSUE_TEMPLATE/errata.md`](../.github/ISSUE_TEMPLATE/errata.md).

## Public API

Everything external consumers should use lives in
[`include/t3.h`](include/t3.h). Headers outside that path are private
and may change without notice.

## Build

```sh
cd build && make            # POSIX Make → libteleproto3.a
bazel build //lib:libteleproto3   # for iOS fork consumption
```

Output: `lib/build/libteleproto3.a` (static), plus the header at
`lib/include/t3.h`.

## Testing

```sh
cd build && make test       # unit + integration
```

- Unit: `tests/unit/` — one `.c` per translation unit, pure.
- Integration: `tests/integration/` — exercises lib-internal timing
  (e.g. AC-PROTO-001 KS test).

Conformance tests live at the repo level, not here. Run
`../conformance/runner/run.sh --impl ./examples/c-client-side` to
exercise the lib against the normative vectors.

## Examples

- [`examples/c-server-side/`](examples/c-server-side/) — minimal server
  integration.
- [`examples/c-client-side/`](examples/c-client-side/) — minimal client
  integration. Used by the in-repo conformance CI as the default IUT.

## Docs

- [`docs/consuming-from-c.md`](docs/consuming-from-c.md) — how to
  integrate from a pure-C codebase.
- [`docs/versioning.md`](docs/versioning.md) — `lib` ↔ `spec` compat
  rules and the cross-version CI gate.
- [`docs/interop-notes.md`](docs/interop-notes.md) — platform gotchas
  (Safari 15, old nginx, CF Worker).

## Licence

Apache 2.0 — see [`LICENSE`](LICENSE).

## Versioning

See [`VERSION`](VERSION) and [`CHANGELOG.md`](CHANGELOG.md). Tags are
prefixed `lib-vX.Y.Z`. The `implements_spec` field in `VERSION`
records the spec version range this library conforms to.
