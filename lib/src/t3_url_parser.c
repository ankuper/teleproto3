/*
 * This is a reference implementation of the Type3 protocol.
 * Normative behaviour is defined in spec/. Where they differ, spec/ wins.
 * File a bug or errata at https://github.com/ankuper/teleproto3/issues.
 */

/*
 * t3_url_parser.c — parses Type3 distribution URLs (tg://proxy... and
 * deep-link variants) into a t3_secret_t. Normative reference:
 * spec/secret-format.md §2.
 *
 * TODO(lib-v0.1.0): implement. Reject all non-canonical encodings with
 * T3_ERR_MALFORMED.
 */

#include "t3.h"
