/*
 * run_kdf_vectors.c — KDF reference runner for story 1-12.
 *
 * Reads KDF Known-Answer Test vectors from kdf-kat.txt (tab-separated:
 *   id  random_header_hex(128)  secret_key_hex(32)
 * skipping comment lines starting with '#').
 *
 * For each vector, computes the Type3 KDF (spec/wire-format.md §4.2)
 * using OpenSSL SHA-256 directly, then SHA-256 hashes the concatenated
 * key material and prints:
 *   <id>  <sha256_of_key_material_hex>
 *
 * This program is the authoritative source of linux-reference.sha256.
 * It does NOT link against libteleproto3 — it implements the KDF from
 * spec to act as an independent reference, not a self-validating test.
 *
 * Usage:
 *   run_kdf_vectors <path-to-kdf-kat.txt>
 *
 * Story 1-12 (AC #5).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include <openssl/sha.h>

#define RANDOM_HEADER_LEN 64
#define SECRET_KEY_LEN    16
#define SHA256_LEN        32

/* hex_decode — convert hex string of length 2*out_len into out[out_len]. */
static int hex_decode(const char *hex, uint8_t *out, size_t out_len) {
    for (size_t i = 0; i < out_len; i++) {
        unsigned hi, lo;
        if (sscanf(hex + 2*i, "%1x%1x", &hi, &lo) != 2) {
            return -1;
        }
        out[i] = (uint8_t)((hi << 4) | lo);
    }
    return 0;
}

/* hex_encode — convert bytes to lower-case hex string (null-terminated). */
static void hex_encode(const uint8_t *in, size_t len, char *out) {
    static const char h[] = "0123456789abcdef";
    for (size_t i = 0; i < len; i++) {
        out[2*i]   = h[(in[i] >> 4) & 0xf];
        out[2*i+1] = h[in[i] & 0xf];
    }
    out[2*len] = '\0';
}

/*
 * t3_kdf_ref — implements the normative KDF from spec/wire-format.md §4.2.
 *
 * Inputs:
 *   rh[64]  — random_header (64 bytes)
 *   sk[16]  — secret_key (16 bytes)
 *
 * Outputs:
 *   read_key[32], read_iv[16], write_key[32], write_iv[16]
 *
 * Algorithm:
 *   read_key  = SHA256(rh[8:40]           || sk)      (32+16 = 48 bytes input)
 *   read_iv   = rh[40:56]                             (16 bytes, direct)
 *   write_key = SHA256(reverse(rh[24:56]) || sk)      (32+16 = 48 bytes input)
 *   write_iv  = reverse(rh[8:24])                     (16 bytes)
 *
 * reverse() reverses byte order only (not bit order).
 */
static void t3_kdf_ref(
        const uint8_t rh[RANDOM_HEADER_LEN],
        const uint8_t sk[SECRET_KEY_LEN],
        uint8_t read_key[SHA256_LEN],
        uint8_t read_iv[16],
        uint8_t write_key[SHA256_LEN],
        uint8_t write_iv[16])
{
    uint8_t buf[SHA256_LEN + SECRET_KEY_LEN]; /* 48-byte SHA input */

    /* read_key = SHA256(rh[8:40] || sk) */
    memcpy(buf,              rh + 8,  32);  /* rh[8..40) = 32 bytes */
    memcpy(buf + 32,         sk,      16);
    SHA256(buf, 48, read_key);

    /* read_iv = rh[40:56] (16 bytes direct) */
    memcpy(read_iv, rh + 40, 16);

    /* write_key = SHA256(reverse(rh[24:56]) || sk) */
    for (int i = 0; i < 32; i++) {
        buf[i] = rh[55 - i]; /* reverse of rh[24..56) */
    }
    memcpy(buf + 32, sk, 16);
    SHA256(buf, 48, write_key);

    /* write_iv = reverse(rh[8:24]) */
    for (int i = 0; i < 16; i++) {
        write_iv[i] = rh[23 - i]; /* reverse of rh[8..24) */
    }
}

int main(int argc, char *argv[]) {
    if (argc != 2) {
        fprintf(stderr, "usage: %s <kdf-kat.txt>\n", argv[0]);
        return 1;
    }

    FILE *fp = fopen(argv[1], "r");
    if (!fp) {
        perror(argv[1]);
        return 1;
    }

    char line[512];
    int failed = 0;

    while (fgets(line, sizeof(line), fp)) {
        /* strip newline */
        line[strcspn(line, "\n\r")] = '\0';

        /* skip blank lines and comments */
        if (line[0] == '\0' || line[0] == '#') {
            continue;
        }

        /* parse: id <tab> rh_hex <tab> sk_hex */
        char *id  = line;
        char *rh_hex = strchr(id, '\t');
        if (!rh_hex) { fprintf(stderr, "bad line: %s\n", line); failed++; continue; }
        *rh_hex++ = '\0';

        char *sk_hex = strchr(rh_hex, '\t');
        if (!sk_hex) { fprintf(stderr, "bad line: %s\n", line); failed++; continue; }
        *sk_hex++ = '\0';

        if (strlen(rh_hex) != 2u * RANDOM_HEADER_LEN) {
            fprintf(stderr, "%s: random_header must be %zu hex chars (got %zu)\n",
                    id, (size_t)(2u * RANDOM_HEADER_LEN), strlen(rh_hex));
            failed++;
            continue;
        }
        if (strlen(sk_hex) != 2u * SECRET_KEY_LEN) {
            fprintf(stderr, "%s: secret_key must be %zu hex chars (got %zu)\n",
                    id, (size_t)(2u * SECRET_KEY_LEN), strlen(sk_hex));
            failed++;
            continue;
        }

        uint8_t rh[RANDOM_HEADER_LEN], sk[SECRET_KEY_LEN];
        if (hex_decode(rh_hex, rh, RANDOM_HEADER_LEN) != 0 ||
            hex_decode(sk_hex, sk, SECRET_KEY_LEN)    != 0) {
            fprintf(stderr, "%s: hex decode error\n", id);
            failed++;
            continue;
        }

        uint8_t read_key[SHA256_LEN], read_iv[16];
        uint8_t write_key[SHA256_LEN], write_iv[16];
        t3_kdf_ref(rh, sk, read_key, read_iv, write_key, write_iv);

        /* SHA256(read_key || read_iv || write_key || write_iv) */
        uint8_t material[SHA256_LEN + 16 + SHA256_LEN + 16]; /* 96 bytes */
        memcpy(material,              read_key,  SHA256_LEN);
        memcpy(material + SHA256_LEN, read_iv,   16);
        memcpy(material + SHA256_LEN + 16, write_key, SHA256_LEN);
        memcpy(material + SHA256_LEN + 16 + SHA256_LEN, write_iv, 16);

        uint8_t digest[SHA256_LEN];
        SHA256(material, sizeof(material), digest);

        char hex_out[SHA256_LEN * 2 + 1];
        hex_encode(digest, SHA256_LEN, hex_out);

        printf("%s  %s\n", id, hex_out);
    }

    fclose(fp);
    return (failed > 0) ? 1 : 0;
}
