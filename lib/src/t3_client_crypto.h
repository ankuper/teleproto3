/*
 * t3_client_crypto.h — internal: obfs2 KDF + AES-CTR for client side.
 */
#ifndef T3_CLIENT_CRYPTO_H
#define T3_CLIENT_CRYPTO_H

#include <stdint.h>
#include <stddef.h>
#include <openssl/evp.h>

/* AES-CTR context wrapper */
typedef struct {
    EVP_CIPHER_CTX *ctx;
} t3c_aes_ctx;

/* Generate 64-byte obfs2 init header and set up AES-CTR contexts.
 *
 * secret:      16-byte proxy secret
 * dc_id:       target DC (signed)
 * header_out:  receives 64-byte header to send over the wire
 * encrypt_ctx: AES-CTR context for encrypting outgoing data (counter starts at 64)
 * decrypt_ctx: AES-CTR context for decrypting incoming data (counter starts at 0)
 *
 * Returns 0 on success, -1 on failure.
 */
int t3c_obfs2_generate_init(const uint8_t secret[16], int16_t dc_id,
                            uint8_t header_out[64],
                            t3c_aes_ctx *encrypt_ctx,
                            t3c_aes_ctx *decrypt_ctx);

/* AES-CTR encrypt or decrypt (symmetric). In-place safe. */
int t3c_aes_crypt(t3c_aes_ctx *ctx, const uint8_t *in, uint8_t *out, size_t len);

/* Free AES context resources. */
void t3c_aes_ctx_free(t3c_aes_ctx *ctx);

#endif /* T3_CLIENT_CRYPTO_H */
