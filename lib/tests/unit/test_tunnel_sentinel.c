/*
 * test_tunnel_sentinel.c — ATDD RED-phase unit scaffold for story 9.2 AC1.
 *
 * Not subject to banner-discipline (tests/, not src/). Returns 0 on pass / 1 on fail,
 * mirroring tests/unit/test_padding.c and tests/unit/test_t3_client.c.
 *
 * Include note: t3c_obfs2_generate_init + t3c_aes_ctx are declared in
 *   lib/src/t3_client_crypto.h  (an internal header that lives under src/, NOT
 *   include/internal/). The existing test_t3_client target already puts src/
 *   on the include path under the T3_BUILD_CLIENT gate, so we include it the
 *   same way test_t3_client.c does. t3c_aes_ctx is { EVP_CIPHER_CTX *ctx; }.
 *
 * ===================================================================
 *  ===== ATDD RED PHASE (story 9.2 AC1) — see checklist =====
 * ===================================================================
 * This target is NOT compiled by the default build. It is gated behind the
 * NEW CMake option T3_ATDD_RED (default OFF) and its CTest entry is marked
 * DISABLED TRUE, so CI stays green.
 *
 * It calls a forward-declared EXPECTED-NEW function
 *   t3c_obfs2_generate_init_tunnel(...)
 * that DOES NOT EXIST YET. When a dev activates this scaffold
 * (configure with -DT3_ATDD_RED=ON, remove the DISABLED property), the build
 * FAILS TO LINK (undefined symbol) until the function is implemented — that
 * link failure is the RED signal. Once implemented to satisfy the asserts
 * below, the test goes GREEN.
 *
 * The exact name/shape of the new function is the implementer's choice; THIS
 * TEST documents the REQUIRED OBSERVABLE BEHAVIOR. If the impl names it
 * differently, adjust the single forward-declaration line below.
 *
 * ---- obfs2 wire reality (do NOT assert plaintext at [56:64] directly) ----
 * t3c_obfs2_generate_init re-encrypts bytes [56:64] in place before returning
 * (t3_client_crypto.c: "Replace bytes 56-63 with encrypted version (obfs2
 * protocol)"). So in the returned header:
 *   - bytes [0:56]  are CLEARTEXT (random preamble + session header + keys),
 *   - bytes [56:64] are AES-256-CTR CIPHERTEXT (the tag + dc_id/sentinel).
 * The tag/sentinel are therefore NOT readable at [56:64] as-is; we recover them
 * with the SAME write key/iv the server derives (recover_plain_init below).
 * This mirrors exactly how the server decodes the init.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include <openssl/evp.h>        /* AES-256-CTR + SHA-256 to recover [56:64] */

#include "t3_client_crypto.h"   /* t3c_obfs2_generate_init, t3c_aes_ctx, t3c_aes_ctx_free */

/* ---------------------------------------------------------------------------
 * FORWARD DECLARATION of the EXPECTED-NEW tunnel-mode init function (AC1).
 *
 * Contract (the observable behavior the test pins):
 *   - Produces the SAME 64-byte obfs2 init header as t3c_obfs2_generate_init,
 *     EXCEPT the dc_id field at header[60:62] is replaced by the SOCKS5-tunnel
 *     sentinel bytes 0x53 0x53 ('S','S').
 *   - The padded-intermediate transport tag 0xdddddddd at header[56:60] is
 *     UNCHANGED.
 *   - No dc_id parameter (the sentinel takes its place).
 *   - Same reserved-prefix reroll / KDF / AES-CTR context setup as the
 *     non-tunnel function. Returns 0 on success, -1 on failure.
 *
 * If the implementer chooses a different name/signature, change ONLY this line.
 * ------------------------------------------------------------------------- */
int t3c_obfs2_generate_init_tunnel(const uint8_t secret[16],
                                   uint8_t header_out[64],
                                   t3c_aes_ctx *encrypt_ctx,
                                   t3c_aes_ctx *decrypt_ctx);

/* SOCKS5-tunnel sentinel that replaces dc_id at header[60:62]. */
#define T3_TUNNEL_SENTINEL_B0  0x53  /* 'S' */
#define T3_TUNNEL_SENTINEL_B1  0x53  /* 'S' */
/* Canonical padded-intermediate transport tag at header[56:60]. */
#define T3_PADDED_INTERMEDIATE_TAG  0xddddddddu

/* Little-endian load of a uint32 from a byte offset (matches the
 * *(uint32_t *)(header + N) writes in t3_client_crypto.c on LE targets;
 * this helper keeps the assert intent explicit and avoids aliasing UB). */
static uint32_t load_u32_le(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8)
         | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static uint16_t load_u16_le(const uint8_t *p) {
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

/* ---------------------------------------------------------------------------
 * Recover the plaintext obfs2 init [56:64] from a returned header, exactly as
 * the server does:
 *   write_key = SHA256(header[8:40] || secret[0:16])   write_iv = header[40:56]
 * then AES-256-CTR over the whole 64-byte header. CTR is a stream cipher, so
 * decrypting [0:64] reproduces plaintext at [56:64] (we ignore [0:56], which is
 * already cleartext and would here be XORed into garbage). plain_out[56:64] then
 * holds tag(0xdddddddd) || dc_id-or-sentinel || reserved.
 * Returns 0 on success, -1 on OpenSSL failure.
 * ------------------------------------------------------------------------- */
static int recover_plain_init(const uint8_t header_out[64],
                              const uint8_t secret[16],
                              uint8_t plain_out[64]) {
    uint8_t kin[48];
    memcpy(kin, header_out + 8, 32);   /* header[8:40] */
    memcpy(kin + 32, secret, 16);      /* || secret    */

    uint8_t write_key[32];
    unsigned int mdlen = 0;
    if (EVP_Digest(kin, sizeof kin, write_key, &mdlen, EVP_sha256(), NULL) != 1)
        return -1;

    EVP_CIPHER_CTX *c = EVP_CIPHER_CTX_new();
    if (!c) return -1;
    int ok = (EVP_DecryptInit_ex(c, EVP_aes_256_ctr(), NULL,
                                 write_key, header_out + 40 /* write_iv */) == 1);
    int outlen = 0;
    if (ok) ok = (EVP_DecryptUpdate(c, plain_out, &outlen, header_out, 64) == 1);
    EVP_CIPHER_CTX_free(c);
    return ok ? 0 : -1;
}

/* ---------------------------------------------------------------------------
 * [P0] AC1 — tunnel-mode init stamps the SOCKS5 sentinel, keeps the tag.
 * RED: links only once t3c_obfs2_generate_init_tunnel() is implemented.
 * ------------------------------------------------------------------------- */
static int test_tunnel_sentinel_stamped(void) {
    printf("[test_tunnel_sentinel_stamped] starting...\n");

    const uint8_t secret[16] = {
        0x78, 0xef, 0x15, 0x1a, 0x20, 0x06, 0x67, 0x70,
        0xdb, 0x00, 0xa2, 0xf9, 0x05, 0xc1, 0x03, 0xe9
    };
    uint8_t header[64];
    t3c_aes_ctx enc, dec;

    int rc = t3c_obfs2_generate_init_tunnel(secret, header, &enc, &dec);
    if (rc != 0) {
        fprintf(stderr, "[test_tunnel_sentinel_stamped] FAIL: init_tunnel rc=%d\n", rc);
        return 1;
    }

    /* Recover the encrypted [56:64] fields (server-side decode). */
    uint8_t plain[64];
    if (recover_plain_init(header, secret, plain) != 0) {
        fprintf(stderr, "[test_tunnel_sentinel_stamped] FAIL: recover_plain_init\n");
        t3c_aes_ctx_free(&enc); t3c_aes_ctx_free(&dec);
        return 1;
    }

    /* Sentinel 0x53 0x53 at [60:62] (decrypted) — NOT a dc_id. */
    if (plain[60] != T3_TUNNEL_SENTINEL_B0 || plain[61] != T3_TUNNEL_SENTINEL_B1) {
        fprintf(stderr,
            "[test_tunnel_sentinel_stamped] FAIL: plain[60:62]=%02x %02x, expected 53 53\n",
            plain[60], plain[61]);
        t3c_aes_ctx_free(&enc); t3c_aes_ctx_free(&dec);
        return 1;
    }

    /* Canonical padded-intermediate tag 0xdddddddd at [56:60] preserved. */
    uint32_t tag = load_u32_le(plain + 56);
    if (tag != T3_PADDED_INTERMEDIATE_TAG) {
        fprintf(stderr,
            "[test_tunnel_sentinel_stamped] FAIL: tag@56=0x%08x, expected 0xdddddddd\n", tag);
        t3c_aes_ctx_free(&enc); t3c_aes_ctx_free(&dec);
        return 1;
    }

    /* Exact 16-bit sentinel word so the decoded shape is unambiguous. */
    if (load_u16_le(plain + 60) != 0x5353u) {
        fprintf(stderr, "[test_tunnel_sentinel_stamped] FAIL: u16@60 != 0x5353\n");
        t3c_aes_ctx_free(&enc); t3c_aes_ctx_free(&dec);
        return 1;
    }

    t3c_aes_ctx_free(&enc);
    t3c_aes_ctx_free(&dec);
    printf("[test_tunnel_sentinel_stamped] PASS\n");
    return 0;
}

/* ---------------------------------------------------------------------------
 * [P1] AC1 regression guard — the EXISTING non-tunnel init is unchanged.
 * Exercises t3c_obfs2_generate_init (which already exists) and decodes [56:64]
 * the same server-side way: dc_id must survive and must NOT be the sentinel.
 * After AC1 lands it must STILL pass — proving normal callers are not regressed.
 * (Depends only on the existing API, so it is the GREEN anchor — but it still
 * needs the T3_ATDD_RED target to be built to run.)
 * ------------------------------------------------------------------------- */
static int test_nontunnel_dcid_preserved(void) {
    printf("[test_nontunnel_dcid_preserved] starting...\n");

    const uint8_t secret[16] = {1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16};
    const int16_t dc_id = 2;
    uint8_t header[64];
    t3c_aes_ctx enc, dec;

    int rc = t3c_obfs2_generate_init(secret, dc_id, header, &enc, &dec);
    if (rc != 0) {
        fprintf(stderr, "[test_nontunnel_dcid_preserved] FAIL: generate_init rc=%d\n", rc);
        return 1;
    }

    uint8_t plain[64];
    if (recover_plain_init(header, secret, plain) != 0) {
        fprintf(stderr, "[test_nontunnel_dcid_preserved] FAIL: recover_plain_init\n");
        t3c_aes_ctx_free(&enc); t3c_aes_ctx_free(&dec);
        return 1;
    }

    /* dc_id (=2) is written verbatim at [60:62] and is NOT the sentinel. */
    int16_t got = (int16_t)load_u16_le(plain + 60);
    if (got != dc_id) {
        fprintf(stderr,
            "[test_nontunnel_dcid_preserved] FAIL: dc_id@60=%d, expected %d\n", got, dc_id);
        t3c_aes_ctx_free(&enc); t3c_aes_ctx_free(&dec);
        return 1;
    }
    if (plain[60] == T3_TUNNEL_SENTINEL_B0 && plain[61] == T3_TUNNEL_SENTINEL_B1) {
        fprintf(stderr,
            "[test_nontunnel_dcid_preserved] FAIL: non-tunnel header carries 0x5353 sentinel\n");
        t3c_aes_ctx_free(&enc); t3c_aes_ctx_free(&dec);
        return 1;
    }

    /* Same canonical tag at [56:60]. */
    uint32_t tag = load_u32_le(plain + 56);
    if (tag != T3_PADDED_INTERMEDIATE_TAG) {
        fprintf(stderr,
            "[test_nontunnel_dcid_preserved] FAIL: tag@56=0x%08x, expected 0xdddddddd\n", tag);
        t3c_aes_ctx_free(&enc); t3c_aes_ctx_free(&dec);
        return 1;
    }

    t3c_aes_ctx_free(&enc);
    t3c_aes_ctx_free(&dec);
    printf("[test_nontunnel_dcid_preserved] PASS\n");
    return 0;
}

/* ---------------------------------------------------------------------------
 * [P2] AC1 edge — obfs2 reserved-prefix reroll still holds on the TUNNEL header.
 * header[0:56] is CLEARTEXT in the returned buffer, so the first-4-bytes checks
 * read the raw header directly (NO decrypt needed). The first 4 bytes must NOT
 * be a reserved/printable-method prefix, header[0] must not be 0xef, and
 * header[4:8] must be non-zero (mirrors the reroll loop in t3_client_crypto.c
 * lines ~46-54). RED until init_tunnel links.
 * ------------------------------------------------------------------------- */
static int test_tunnel_reserved_prefix_valid(void) {
    printf("[test_tunnel_reserved_prefix_valid] starting...\n");

    const uint8_t secret[16] = {
        0xaa, 0xbb, 0xcc, 0xdd, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
    };
    uint8_t header[64];
    t3c_aes_ctx enc, dec;

    int rc = t3c_obfs2_generate_init_tunnel(secret, header, &enc, &dec);
    if (rc != 0) {
        fprintf(stderr, "[test_tunnel_reserved_prefix_valid] FAIL: init_tunnel rc=%d\n", rc);
        return 1;
    }

    int fail = 0;
    uint32_t first = load_u32_le(header);          /* header[0:4] — cleartext */
    uint32_t at4   = load_u32_le(header + 4);       /* header[4:8] — cleartext */

    if (header[0] == 0xef) { fprintf(stderr, "  FAIL: header[0]==0xef\n"); fail = 1; }
    if (first == 0x44414548u) { fprintf(stderr, "  FAIL: prefix 'HEAD'\n"); fail = 1; }
    if (first == 0x54534f50u) { fprintf(stderr, "  FAIL: prefix 'POST'\n"); fail = 1; }
    if (first == 0x20544547u) { fprintf(stderr, "  FAIL: prefix 'GET '\n"); fail = 1; }
    if (first == 0x4954504fu) { fprintf(stderr, "  FAIL: prefix 'OPTI'\n"); fail = 1; }
    if (first == 0xeeeeeeeeu) { fprintf(stderr, "  FAIL: prefix 0xeeeeeeee\n"); fail = 1; }
    if (first == 0xddddddddu) { fprintf(stderr, "  FAIL: prefix 0xdddddddd\n"); fail = 1; }
    if (first == 0xefefefefu) { fprintf(stderr, "  FAIL: prefix 0xefefefef\n"); fail = 1; }
    if (at4 == 0x00000000u)   { fprintf(stderr, "  FAIL: header[4:8]==0\n"); fail = 1; }

    t3c_aes_ctx_free(&enc);
    t3c_aes_ctx_free(&dec);
    if (fail) { fprintf(stderr, "[test_tunnel_reserved_prefix_valid] FAIL\n"); return 1; }
    printf("[test_tunnel_reserved_prefix_valid] PASS\n");
    return 0;
}

int main(void) {
    int rc = 0;
    printf("\n=== story 9.2 AC1 tunnel-sentinel unit tests (ATDD RED) ===\n\n");
    rc |= test_tunnel_sentinel_stamped();      /* P0 */
    rc |= test_nontunnel_dcid_preserved();     /* P1 (green anchor) */
    rc |= test_tunnel_reserved_prefix_valid(); /* P2 */
    printf("\n=== done (rc=%d) ===\n\n", rc);
    return rc;
}
