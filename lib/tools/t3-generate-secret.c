/*
 * t3-generate-secret — CLI utility for generating Type3 secrets.
 *
 * Usage:
 *   t3-generate-secret <host> [<path>]
 *
 * Examples:
 *   t3-generate-secret arctic-breeze.my.id /ws/7f34ba
 *   t3-generate-secret example.com
 *
 * Output (stdout): full hex secret (ff + key + domain)
 * Diagnostics (stderr): key, host, path, type
 */

#include "t3.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Platform-specific CSPRNG for key generation. */
#if defined(__APPLE__)
#  include <Security/SecRandom.h>
static int fill_random(uint8_t *buf, size_t len) {
    return SecRandomCopyBytes(kSecRandomDefault, len, buf) == errSecSuccess ? 0 : -1;
}
#elif defined(_WIN32)
#  include <bcrypt.h>
#  pragma comment(lib, "bcrypt")
static int fill_random(uint8_t *buf, size_t len) {
    return BCryptGenRandom(NULL, buf, (ULONG)len, BCRYPT_USE_SYSTEM_PREFERRED_RNG) == 0 ? 0 : -1;
}
#else
#  include <fcntl.h>
#  include <unistd.h>
static int fill_random(uint8_t *buf, size_t len) {
    int fd = open("/dev/urandom", O_RDONLY);
    if (fd < 0) return -1;
    ssize_t r = read(fd, buf, len);
    close(fd);
    return (r == (ssize_t)len) ? 0 : -1;
}
#endif

int main(int argc, char *argv[]) {
    if (argc < 2) {
        fprintf(stderr, "Usage: %s <host> [<path>]\n", argv[0]);
        fprintf(stderr, "\nExamples:\n");
        fprintf(stderr, "  %s arctic-breeze.my.id /ws/7f34ba\n", argv[0]);
        fprintf(stderr, "  %s example.com\n", argv[0]);
        return 1;
    }

    const char *host = argv[1];
    const char *path = (argc >= 3) ? argv[2] : NULL;

    /* Validate host and path via libteleproto3 API. */
    t3_result_t rc = t3_secret_validate_host(host);
    if (rc != T3_OK) {
        fprintf(stderr, "Error: invalid host '%s': %s\n", host, t3_strerror(rc));
        return 1;
    }
    if (path) {
        rc = t3_secret_validate_path(path);
        if (rc != T3_OK) {
            fprintf(stderr, "Error: invalid path '%s': %s\n", path, t3_strerror(rc));
            return 1;
        }
    }

    /* Generate 16 random key bytes. */
    t3_secret_fields fields;
    memset(&fields, 0, sizeof(fields));
    if (fill_random(fields.key, 16) != 0) {
        fprintf(stderr, "Error: CSPRNG failure\n");
        return 1;
    }
    fields.host = host;
    fields.path = path;

    /* Serialise via libteleproto3 API. */
    uint8_t buf[1024];
    size_t buf_len = sizeof(buf);
    rc = t3_secret_serialise(&fields, buf, &buf_len);
    if (rc != T3_OK) {
        fprintf(stderr, "Error: serialise failed: %s\n", t3_strerror(rc));
        t3_secret_zeroise(&fields);
        return 1;
    }

    /* Print full hex secret to stdout. */
    for (size_t i = 0; i < buf_len; i++) {
        printf("%02x", buf[i]);
    }
    printf("\n");

    /* Diagnostics to stderr. */
    char key_hex[33];
    for (int i = 0; i < 16; i++) {
        snprintf(key_hex + i * 2, 3, "%02x", fields.key[i]);
    }
    key_hex[32] = '\0';

    fprintf(stderr, "Secret for -S:  %s\n", key_hex);
    fprintf(stderr, "Host:           %s\n", host);
    if (path) {
        fprintf(stderr, "Path:           %s\n", path);
    }
    fprintf(stderr, "Type:           Type3/WS\n");

    t3_secret_zeroise(&fields);
    return 0;
}
