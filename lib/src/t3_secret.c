/*
 * This is a reference implementation of the Type3 protocol.
 * Normative behaviour is defined in spec/. Where they differ, spec/ wins.
 * File a bug or errata at https://github.com/ankuper/teleproto3/issues.
 */

/*
 * t3_secret.c — implements the secret-format parsing / serialisation
 * API declared in include/t3.h. Normative reference:
 * spec/secret-format.md.
 *
 * TODO(lib-v0.1.0): implement t3_secret_parse / t3_secret_free per
 * spec/secret-format.md §1–3.
 */

#include "t3.h"

/* Placeholder; real symbols land in lib-v0.1.0. */
