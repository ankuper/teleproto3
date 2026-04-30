/*
 * test_obfuscated2.c — placeholder unit tests (obfuscated-2 not implemented at v0.1.0).
 * Not subject to banner-discipline (tests/, not src/).
 * Source: story 1.7 Task 8.
 */

#include "t3.h"
#include <stdio.h>

int main(void) {
    /* Placeholder: obfuscated-2 implementation is deferred (stub src).
     * The conformance vectors for obfuscated-handshake use TODO-KAT values.
     * This test passes as long as the library links correctly. */
    const char *ver = t3_abi_version_string();
    printf("[obfuscated2] ABI version: %s — placeholder PASS\n", ver);
    return 0;
}
