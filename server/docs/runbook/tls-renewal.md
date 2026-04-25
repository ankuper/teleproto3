# Runbook: TLS renewal

nginx fronts mtproto-proxy and terminates TLS. Certificates are
Let's Encrypt, issued against the nip.io-style domain baked into the
Type3 secret.

## When

- Certbot auto-renew runs twice daily by default and renews within 30
  days of expiry — no human action in the happy path.
- **Page condition:** a renewal failure that leaves <7 days to expiry,
  OR a mismatch between the cert SNI and the Type3-secret-domain.

## What to check first

1. `certbot certificates` — list all managed certs and their expiry.
2. `systemctl status certbot.timer` — confirm the timer is active.
3. `nginx -t && systemctl reload nginx` — verify nginx reloads cleanly
   with the renewed cert in place.
4. Compare the cert SNI to the domain encoded in the live Type3
   secrets (`/etc/mtproto-proxy/secrets.conf` or equivalent). Mismatch
   means the client will see a degraded state until secrets are
   rotated.

## Manual renewal

```sh
certbot renew --cert-name <domain>
nginx -t && systemctl reload nginx
```

## Edge cases

- _TBD(server-v0.1.0):_ DNS-01 vs HTTP-01 pitfalls specific to nip.io.
- _TBD:_ when rotating the underlying domain is better than renewing.

## Rollback

_TBD._ Restoring a previous cert from `/etc/letsencrypt/archive/`.
