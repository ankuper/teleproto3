# Extended scenarios — Extended compliance

Reserved for future / optional features. Claiming **Extended** without
first passing Full is not permitted.

## Scope

_TBD._ Candidates for future Extended scenarios:

- Experimental probe heuristics beyond the mandatory silent-close.
- Alternate framing modes (if a future spec MAJOR introduces them).
- Post-quantum key-derivation variants.
- Advanced diagnostics surface (admin hooks not required for Core/Full).

Scaffold state: empty at conformance-v0.1.0-draft. Content lands when
the corresponding spec sections do.

## Scenarios in this level

Catalogue of scenarios assigned to the `extended/` directory at
conformance-v0.1.0-draft (placeholder list):

- `proxy-protocol-e2e.md` — <!-- ban-list-doc: technical identifier -->PROXY-protocol<!-- /ban-list-doc --> v1/v2 end-to-end scenario
  stub (status: draft). Owned by Epic 2 story 2.10. CDN-fronted
  deployments only (Epic 1 §4). Do NOT populate here.

Additional extended scenarios TBD as corresponding spec sections land.

## How this level is invoked by runner

`run.sh --level extended` runs `mandatory/`, `full/`, **and** this
directory. `--level core` and `--level full` do NOT run this directory.
