/*
 * This is a reference implementation of the Type3 protocol.
 * Normative behaviour is defined in spec/. Where they differ, spec/ wins.
 * File a bug or errata at https://github.com/ankuper/teleproto3/issues.
 */

/*
 * t3_keyderiv.c — derives AES-CTR key/IV material from the pre-shared
 * 16-byte secret. Normative reference: spec/wire-format.md §4.
 *
 * TODO(lib-v0.1.0): implement with constant-time primitives only.
 */

#include "t3.h"
