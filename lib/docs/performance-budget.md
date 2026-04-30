# Performance Budget — libteleproto3 v0.1.0

This is the reference implementation of the Type3 protocol.
Normative behaviour is defined in spec/. Where they differ, spec/ wins.

Source: story 1.7 AC10. Stability: lib-v0.1.0 ABI.

## Per-Operation Microbenchmark Budgets

Measured on the gating CI host under `-O2 -DNDEBUG -fPIC -fvisibility=hidden`.
All budgets are **worst-case wall-clock** ceilings, not averages.

| Operation                              | Budget     | Notes                                          |
|----------------------------------------|------------|------------------------------------------------|
| `t3_header_parse`                      | ≤ 200 ns   | 4-byte LE decode + validation                 |
| `t3_header_serialise`                  | ≤ 200 ns   | 4-byte LE encode                               |
| `t3_secret_parse`                      | ≤ 5 µs     | Includes UTF-8 scan + path split               |
| `t3_silent_close_delay_sample_ns`      | ≤ 1 µs     | Rejection sampling loop (amortised)            |
| `t3_retry_record_close`               | ≤ 500 ns   | Ring eviction + FSM transition                 |
| `t3_secret_free`                       | ≤ 200 ns   | Volatile zeroisation + free                    |
| `t3_session_new`                       | ≤ 1 µs     | Single calloc + field init                     |
| `t3_session_bind_callbacks`            | ≤ 200 ns   | memcpy of callback struct                      |
| `t3_strerror`                          | ≤ 50 ns    | Switch lookup                                  |
| `t3_abi_version_string`                | ≤ 50 ns    | Static string return                           |

## Timing Engine Budget

The silent-close timing engine (`t3_silent_close_delay_sample_ns`) uses
rejection sampling to produce uniform-random delays in [50 ms, 200 ms].
The per-call budget of ≤ 1 µs accounts for the rejection loop amortisation
(expected iterations: ≤ 2 for a 150 ms range with 64-bit RNG).

## Baseline Collection

Run `make -C lib/build baseline` to collect p50/p95/p99 latency data.
The baseline YAML is written to `lib/build/timing-baseline.yaml`.
