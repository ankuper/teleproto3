# Deployment

Build, configure, and run the teleproto3 server in front of nginx.
Operational companion to the in-repo [`mtproxy3-architecture.md`](mtproxy3-architecture.md).

> Note: there is a sibling document at `../../../docs/deployment.md`
> (pre-monorepo). That file is the authoritative source until this one
> is filled in during the server-v0.1.0 epic, after which this file
> supersedes it.

## 1. Build

See root-level [`README.md`](../README.md). Quick path:

```sh
make -j$(nproc)
# or
docker build -t teleproto3-server:dev .
```

## 2. Topology

```
Client ── TLS+WS ──▶ nginx (TLS terminator) ──▶ mtproto-proxy :3129 ──▶ Telegram DCs
```

nginx terminates TLS, proxies WS to mtproto-proxy on `127.0.0.1:3129`.
MTProxy handles WS framing internally (not stripped by nginx).

## 3. nginx config

_TBD(server-v0.1.0):_ canonical nginx snippet (upgrade headers,
`proxy_read_timeout`, `proxy_buffering off`, etc).

## 4. Server config

_TBD(server-v0.1.0):_ command-line flags, Type3 secret format
(`0xff` + 16-byte key + domain, UTF-8), multi-secret coexistence
(v1 + v2).

## 5. GHCR image

Published by `.github/workflows/build-server.yml` on `server-v*` tags:

```
ghcr.io/ankuper/teleproto3-server:server-vX.Y.Z
```

## 6. Runbooks

Operational procedures live under [`runbook/`](runbook/):
- [`tls-renewal.md`](runbook/tls-renewal.md)
- [`kill-switch-activation.md`](runbook/kill-switch-activation.md)
- [`migration-from-v1.md`](runbook/migration-from-v1.md)
- [`coexistence.md`](runbook/coexistence.md)

Release cadence: [`deploy/release-cadence.md`](deploy/release-cadence.md).
