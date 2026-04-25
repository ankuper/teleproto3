# MTProxy fork with Type3 dispatch

Fork of the official Telegram MTProxy with the Type3/mtProxy3 transport
(MTProto-over-WebSocket) bolted on. Built and distributed as a GHCR
image: `ghcr.io/ankuper/teleproto3-server:<server-vX.Y.Z>`.

## Structure

- `common/`, `crypto/`, `jobs/`, `mtproto/` — **upstream**, managed via
  `git subtree`. Do not edit directly; pull from the active upstream
  `teleproxy/teleproxy` and log in [`UPSTREAM.md`](UPSTREAM.md).
- `net/` — upstream + fork-local additions (Type3 dispatch, stats).
  The Type3 dispatch hook calls into `../lib/` through `t3.h`.
- `tests/` — server-level integration tests, shell-driven.
- `docs/` — deployment, architecture, runbooks.

## Build

```sh
make -j$(nproc)                   # links ../lib/build/libteleproto3.a
# or:
docker build -t teleproto3-server:dev .
```

Output: `objs/bin/mtproto-proxy`.

## Dependencies

- OpenSSL, zlib (upstream).
- `../lib/build/libteleproto3.a` — built first via `make -C ../lib/build`.

## Licence

Inherits MTProxy upstream licence — see [`LICENSE`](LICENSE). Fork-local
additions under `net/net-type3-*` are contributed under the same
licence.

## Versioning

Independent from spec/lib — tagged `server-vX.Y.Z`. Each release
records the `lib-vX.Y.Z` it was built against via the `linked_lib`
field in `VERSION`.

## Runbooks

See [`docs/runbook/`](docs/runbook/): TLS renewal, kill-switch, v1→v2
migration, v1+v2 coexistence.
