# c-server-side example

Minimal C integration showing how a server embeds libteleproto3.
Consumed by the `server/` fork's hook in `net-type3-dispatch.c`.

## Build

```sh
cc -I../../include -o server_example server_example.c ../../build/libteleproto3.a
```

TODO(lib-v0.1.0): add `server_example.c` demonstrating:
1. Parsing a secret from the server config.
2. Initialising a session from an incoming WS connection.
3. Decoding / re-encoding frames via the public API.
