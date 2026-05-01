---
doc_version: 0.1.0-draft
last_updated: 2026-05-01
status: draft
---
> **Operational contract.** This document defines locked TLS fingerprint profiles
> for Type3 client implementations. Where this document and a client build differ,
> this document wins; the client build is updated (or the profile is refreshed via
> the capture procedure in story 2.1 Dev Notes), not the contract.

# Type3 Client TLS Fingerprint Profiles

TLS fingerprints are locked per client platform × TLS stack combination.
Drift in JA3 or JA4 against a committed profile MUST fail CI.
Refreshing a profile (e.g. after a Qt or OpenSSL minor-version bump) requires:
1. A new PCAP capture following the procedure in story 2.1 Dev Notes.
2. Recomputation of JA3/JA4 from the fresh PCAP.
3. A single commit updating both the PCAP fixture and this document.

---

## §profile-β-qt — tdesktop (Qt + OpenSSL, Linux/macOS/Windows desktop)

**Captured from:** `capture-clienthello` driver (`QSslSocket::connectToHostEncrypted`) against
`94.156.131.252.nip.io:443` from test-client `192.168.30.191`, `tcpdump -i enp1s0f0`, single
`ClientHello` frame extracted via `tshark`.

**Qt version:** 6.9.2  
**OpenSSL version:** 3.5.3  
**Platform:** Linux x86-64 (Ubuntu 25.10); cipher/extension list is Qt+OpenSSL-determined

**Fixture:** `tdesktop/tests/fixtures/qt-openssl-clienthello.pcap`

### Locked values

```
JA3:  95c566c92c1aad5bf391aa24219700ec
JA4:  t13d5612_0000_c19c562a80bc_b7756960ef11
```

### JA3 derivation

JA3 string (input to MD5):

```
769,4866-4867-4865-49196-49200-159-52393-52392-52394-49195-49199-158-49188-49192-107-49187-49191-103-49162-49172-57-49161-49171-51-173-171-52398-52397-52396-157-169-52395-172-170-156-168-61-60-49208-49206-183-179-149-145-53-175-141-49207-49205-182-178-148-144-47-174-140,65281-0-11-10-35-22-23-13-43-45-51-27,4588-29-23-30-24-25-256-257,0-1-2
```

Field breakdown:

| Field | Value |
|---|---|
| SSLVersion | `769` (0x0303, TLS 1.2 compat `legacy_version`) |
| CipherSuites (56) | `4866-4867-4865-49196-49200-159-52393-52392-52394-49195-49199-158-49188-49192-107-49187-49191-103-49162-49172-57-49161-49171-51-173-171-52398-52397-52396-157-169-52395-172-170-156-168-61-60-49208-49206-183-179-149-145-53-175-141-49207-49205-182-178-148-144-47-174-140` |
| Extensions (in order, 12) | `65281-0-11-10-35-22-23-13-43-45-51-27` |
| EllipticCurves (supported_groups, 8) | `4588-29-23-30-24-25-256-257` (X25519MLKEM768, x25519, secp256r1, x448, secp384r1, secp521r1, ffdhe2048, ffdhe3072) |
| ECPointFormats | `0-1-2` (uncompressed, compressed-prime, compressed-char2) |

GREASE values excluded. MD5 of the comma-semicolon-delimited string above.

### JA4 derivation

| Component | Value | Notes |
|---|---|---|
| Protocol | `t` | TCP/TLS |
| TLS version | `13` | TLS 1.3 (from `supported_versions` extension) |
| SNI flag | `d` | domain SNI present (`94.156.131.252.nip.io`) |
| Cipher count | `56` | 56 suites, GREASE excluded |
| Extension count | `12` | 12 extensions, GREASE excluded |
| ALPN first value | `0000` | no ALPN extension in this `ClientHello` |
| Cipher hash | `c19c562a80bc` | first 12 chars of SHA-256 of sorted cipher IDs (hex, comma-separated) |
| Extension hash | `b7756960ef11` | first 12 chars of SHA-256 of sorted ext type IDs, SNI (0) excluded from hash |

Sorted ciphers (hex, for hash):
`002f,0033,0035,0039,003c,003d,0067,006b,008c,008d,0090,0091,0094,0095,009c,009d,009e,009f,00a8,00a9,00aa,00ab,00ac,00ad,00ae,00af,00b2,00b3,00b6,00b7,1301,1302,1303,c009,c00a,c013,c014,c023,c024,c027,c028,c02b,c02c,c02f,c030,c035,c036,c037,c038,cca8,cca9,ccaa,ccab,ccac,ccad,ccae`

Sorted ext types for hash (SNI=0 excluded):
`11,13,22,23,27,35,43,45,51,65281`

> Note: `renegotiation_info` (65281) sorts last numerically. `compress_certificate` (27) is
> included. No ALPN (16) present, so no ALPN exclusion applies.
