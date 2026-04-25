# c-client-side example

Minimal C integration showing how a client embeds libteleproto3.
Also serves as the default IUT (implementation-under-test) for the
in-repo conformance harness CI.

## Build

```sh
cc -I../../include -o client_example client_example.c ../../build/libteleproto3.a
```

## Run under the harness

```sh
../../../conformance/runner/run.sh --impl ./client_example
```

TODO(lib-v0.1.0): add `client_example.c` that speaks the
stdin/stdout harness protocol from `spec/conformance-procedure.md §1`.
