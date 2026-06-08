/*
 * t3_client_crypto.c — obfuscated2 KDF and AES-256-CTR for client-side.
 *
 * Ported from server/src/net/net-obfs2-parse.c and
 * server/src/net/net-tcp-direct-dc.c (tcp_direct_dc_send_obfs2_init).
 *
 * Client-side perspective:
 *   - Client generates 64-byte random header
 *   - Client derives write_key from header[8:40]+secret, write_iv from header[40:56]
 *   - Client derives read_key from reversed header bytes + secret
 *   - Client encrypts all 64 bytes, replaces header[56:64] with encrypted
 *   - Client sends the 64-byte header
 *   - Ongoing: AES-256-CTR encrypt outgoing, decrypt incoming
 *
 * Key naming convention (CLIENT perspective — opposite of server):
 *   Client write (encrypt outgoing) = server read
 *   Client read (decrypt incoming) = server write
 */

#include "t3_client_crypto.h"

#include <string.h>
#include <openssl/evp.h>
#include <openssl/rand.h>
#include <openssl/sha.h>

/* ── SHA-256 helper ─────────────────────────────────────────────────── */
static void sha256_hash(const void *data, size_t len, unsigned char out[32]) {
    SHA256((const unsigned char *)data, len, out);
}

/* ── obfs2 init generation (client side) ────────────────────────────── */

int t3c_obfs2_generate_init(const uint8_t secret[16], int16_t dc_id,
                            uint8_t header_out[64],
                            t3c_aes_ctx *encrypt_ctx,
                            t3c_aes_ctx *decrypt_ctx) {
    uint8_t header[64];

    /* Generate 64 random bytes, avoiding reserved prefixes */
    do {
        if (RAND_bytes(header, 64) != 1) {
            return -1;  /* RNG failure */
        }
    } while (
        header[0] == 0xef ||
        *(uint32_t *)header == 0x44414548 ||   /* "HEAD" */
        *(uint32_t *)header == 0x54534f50 ||   /* "POST" */
        *(uint32_t *)header == 0x20544547 ||   /* "GET " */
        *(uint32_t *)header == 0x4954504f ||   /* "OPTI" */
        *(uint32_t *)header == 0xeeeeeeee ||
        *(uint32_t *)header == 0xdddddddd ||
        *(uint32_t *)header == 0xefefefef ||
        *(uint32_t *)(header + 4) == 0x00000000
    );

    /* Type3 Session Header at bytes [0:3] (matches TDLib Type3HttpStreamTransport) */
    header[0] = 0x01;  /* command_type = MTPROTO_PASSTHROUGH */
    header[1] = 0x01;  /* version = 1 */
    /* flags: T3_FLAG_PADDING (0x0001) — padded intermediate mode */
    header[2] = 0x01;
    header[3] = 0x00;
    /* Set transport tag: padded intermediate (0xdddddddd) at offset 56 */
    *(uint32_t *)(header + 56) = 0xdddddddd;
    /* Set target DC at offset 60 */
    *(int16_t *)(header + 60) = dc_id;

    /* ── Derive client write key (= server read key) ─────────────── */
    /* write_key = SHA256(header[8:40] + secret) */
    uint8_t write_key[32];
    uint8_t write_iv[16];
    {
        uint8_t k[48];
        memcpy(k, header + 8, 32);
        memcpy(k + 32, secret, 16);
        sha256_hash(k, 48, write_key);
    }
    memcpy(write_iv, header + 40, 16);

    /* ── Derive client read key (= server write key) ─────────────── */
    /* read_key bytes are reversed from header, then SHA256(reversed + secret) */
    uint8_t read_key[32];
    uint8_t read_iv[16];
    {
        uint8_t raw_key[32];
        for (int i = 0; i < 32; i++) {
            raw_key[i] = header[55 - i];
        }
        for (int i = 0; i < 16; i++) {
            read_iv[i] = header[23 - i];
        }
        uint8_t k[48];
        memcpy(k, raw_key, 32);
        memcpy(k + 32, secret, 16);
        sha256_hash(k, 48, read_key);
    }

    /* ── Encrypt entire 64 bytes with write key ──────────────────── */
    uint8_t encrypted[64];
    EVP_CIPHER_CTX *tmp = EVP_CIPHER_CTX_new();
    if (!tmp) return -1;
    if (EVP_EncryptInit_ex(tmp, EVP_aes_256_ctr(), NULL, write_key, write_iv) != 1) {
        EVP_CIPHER_CTX_free(tmp);
        return -1;
    }
    int outlen = 0;
    EVP_EncryptUpdate(tmp, encrypted, &outlen, header, 64);

    /* Replace bytes 56-63 with encrypted version (obfs2 protocol) */
    memcpy(header + 56, encrypted + 56, 8);
    memcpy(header_out, header, 64);

    /* ── Set up ongoing AES-CTR contexts ─────────────────────────── */
    /* Write context: counter already at position 64 from tmp */
    encrypt_ctx->ctx = tmp;  /* reuse — counter is at byte 64 */

    /* Read context: fresh, starts at position 0 */
    decrypt_ctx->ctx = EVP_CIPHER_CTX_new();
    if (!decrypt_ctx->ctx) {
        EVP_CIPHER_CTX_free(tmp);
        return -1;
    }
    if (EVP_DecryptInit_ex(decrypt_ctx->ctx, EVP_aes_256_ctr(), NULL, read_key, read_iv) != 1) {
        EVP_CIPHER_CTX_free(decrypt_ctx->ctx);
        EVP_CIPHER_CTX_free(tmp);
        return -1;
    }
    EVP_CIPHER_CTX_set_padding(decrypt_ctx->ctx, 0);

    return 0;
}

/* ── AES-CTR encrypt/decrypt (in-place safe) ────────────────────────── */

int t3c_aes_crypt(t3c_aes_ctx *ctx, const uint8_t *in, uint8_t *out, size_t len) {
    if (!ctx || !ctx->ctx || len == 0) return -1;
    int outlen = 0;
    if (EVP_CipherUpdate(ctx->ctx, out, &outlen, in, (int)len) != 1) {
        return -1;
    }
    return 0;
}

void t3c_aes_ctx_free(t3c_aes_ctx *ctx) {
    if (ctx && ctx->ctx) {
        EVP_CIPHER_CTX_free(ctx->ctx);
        ctx->ctx = NULL;
    }
}
