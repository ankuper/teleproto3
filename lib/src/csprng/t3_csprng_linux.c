/*
 * t3_csprng_linux.c — Linux CSPRNG backend.
 *
 * Primary:  getrandom(2) with GRND_NONBLOCK (glibc ≥2.25, kernel ≥3.17).
 * Fallback: /dev/urandom read loop on ENOSYS (older kernels).
 *
 * Story 1-12 implementation.
 */

#if !defined(__linux__)
#error "t3_csprng_linux.c must only be compiled on Linux"
#endif

#define _GNU_SOURCE
#include <sys/random.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <stdint.h>
#include <stddef.h>

#include "t3.h"
#include "t3_csprng.h"

/* urandom_fill — /dev/urandom fallback; loops on partial reads. */
static t3_result_t urandom_fill(uint8_t *buf, size_t len) {
    int fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        return T3_ERR_RNG;
    }
    size_t done = 0;
    while (done < len) {
        ssize_t n = read(fd, buf + done, len - done);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            close(fd);
            return T3_ERR_RNG;
        }
        if (n == 0) {
            close(fd);
            return T3_ERR_RNG;
        }
        done += (size_t)n;
    }
    close(fd);
    return T3_OK;
}

t3_result_t t3_csprng_bytes(uint8_t *buf, size_t len) {
    size_t done = 0;
    while (done < len) {
        ssize_t n = getrandom(buf + done, len - done, GRND_NONBLOCK);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            if (errno == ENOSYS) {
                /* Kernel predates getrandom(2) — fall back to /dev/urandom. */
                return urandom_fill(buf + done, len - done);
            }
            if (errno == EAGAIN) {
                /*
                 * Entropy pool not yet seeded (very early boot).
                 * Return error immediately to prevent returning weak entropy.
                 */
                return T3_ERR_RNG;
            }
            return T3_ERR_RNG;
        }
        done += (size_t)n;
    }
    return T3_OK;
}
