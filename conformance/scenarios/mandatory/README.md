# Mandatory scenarios — Core compliance

An IUT claiming **Core** compliance MUST pass every scenario under
this directory. Scenarios here cover the minimal-viable-Type3 surface:
secret parsing, one happy-path handshake, mandatory silent-close
behaviour.

## Manifest

_TBD(conformance-v0.1.0):_ each scenario is a YAML/JSON manifest
pointing to the vectors it consumes. Seed list:

- `parse-valid-secret.yaml` — exhaustive accept cases from
  `../../vectors/unit.json#secret-format`.
- `reject-malformed-secret.yaml` — exhaustive reject cases.
- `handshake-happy-path.yaml` — successful handshake trace.
- `anti-probe-silent-close.yaml` — malformed Session Header
  produces silent close (timing + absence of protocol error bytes).

The runner walks this directory, dispatches each manifest to the
IUT through the stdin/stdout protocol, and collects PASS/FAIL.
