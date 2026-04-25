/*
 * This is a reference implementation of the Type3 protocol.
 * Normative behaviour is defined in spec/. Where they differ, spec/ wins.
 * File a bug or errata at https://github.com/ankuper/teleproto3/issues.
 */

/*
 * t3_obfuscated2.c — AES-256-CTR obfuscated-2 framing inside WebSocket
 * frames. Normative reference: spec/wire-format.md §4.
 *
 * TODO(lib-v0.1.0): implement. Coordinates with server/net/net-tcp-
 * connections.c on the server side.
 */

#include "t3.h"
