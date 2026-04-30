/*
 * side_channel.c — g_cb initialisation and timing-side-channel emission (story 1-10).
 *
 * This is the reference implementation of the Type3 protocol.
 * Normative behaviour is defined in spec/. Where they differ, spec/ wins.
 *
 * Stability: internal to lib/fuzz/; not a public ABI surface.
 */

/* Clock rules (epic-1-style-guide.md §11):
 *   Linux : clock_gettime(CLOCK_MONOTONIC)
 *   macOS : clock_gettime_nsec_np(CLOCK_MONOTONIC)
 * NEVER CLOCK_MONOTONIC_RAW, CLOCK_REALTIME, gettimeofday, rdtsc. */

#include "side_channel.h"

#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#if defined(__linux__)
#  include <time.h>
#elif defined(__APPLE__)
#  include <time.h>      /* clock_gettime_nsec_np */
#else
#  error "Unsupported platform — extend per epic-1-style-guide §11"
#endif

/* ------------------------------------------------------------------ */
/* SHA-256 — portable stdlib-only implementation                        */
/* Used only for the side-channel log field (not a security primitive). */
/* ------------------------------------------------------------------ */

static uint32_t sha256_k[64] = {
    0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,
    0x923f82a4,0xab1c5ed5,0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,
    0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,0xe49b69c1,0xefbe4786,
    0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
    0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,
    0x06ca6351,0x14292967,0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,
    0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,0xa2bfe8a1,0xa81a664b,
    0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
    0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,
    0x5b9cca4f,0x682e6ff3,0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,
    0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2
};

#define RR(x,n) (((x)>>(n))|((x)<<(32-(n))))
#define CH(x,y,z) (((x)&(y))^(~(x)&(z)))
#define MAJ(x,y,z) (((x)&(y))^((x)&(z))^((y)&(z)))
#define S0(x) (RR(x,2)^RR(x,13)^RR(x,22))
#define S1(x) (RR(x,6)^RR(x,11)^RR(x,25))
#define G0(x) (RR(x,7)^RR(x,18)^((x)>>3))
#define G1(x) (RR(x,17)^RR(x,19)^((x)>>10))

static void sha256_hash(const uint8_t *msg, size_t len, uint8_t digest[32]) {
    uint32_t h[8] = {
        0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,
        0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19
    };
    /* Append padding */
    uint8_t buf[128];
    memset(buf, 0, sizeof(buf));
    size_t bl = len % 64;
    memcpy(buf, msg + (len - bl), bl);
    buf[bl] = 0x80;
    uint64_t bits = (uint64_t)len * 8;

    /* Determine number of padding blocks.
     * If tail + 0x80 + 8-byte length fits in one 64-byte block (bl < 56),
     * we need 1 padding block.  Otherwise we need 2: the first carries
     * the tail + 0x80 (no length), the second carries zeros + length. */
    size_t full_blocks = len / 64;
    size_t pad_blocks;
    if (bl >= 56) {
        /* Two padding blocks: length goes into second block at offset 120..127. */
        buf[120] = (uint8_t)(bits >> 56);
        buf[121] = (uint8_t)(bits >> 48);
        buf[122] = (uint8_t)(bits >> 40);
        buf[123] = (uint8_t)(bits >> 32);
        buf[124] = (uint8_t)(bits >> 24);
        buf[125] = (uint8_t)(bits >> 16);
        buf[126] = (uint8_t)(bits >>  8);
        buf[127] = (uint8_t)(bits      );
        pad_blocks = 2;
    } else {
        /* One padding block: length at offset 56..63. */
        buf[56] = (uint8_t)(bits >> 56);
        buf[57] = (uint8_t)(bits >> 48);
        buf[58] = (uint8_t)(bits >> 40);
        buf[59] = (uint8_t)(bits >> 32);
        buf[60] = (uint8_t)(bits >> 24);
        buf[61] = (uint8_t)(bits >> 16);
        buf[62] = (uint8_t)(bits >>  8);
        buf[63] = (uint8_t)(bits      );
        pad_blocks = 1;
    }

    /* Process every complete 64-byte block in msg, then the padding block(s). */
    size_t total_blocks = full_blocks + pad_blocks;
    for (size_t blk = 0; blk < total_blocks; blk++) {
        const uint8_t *b;
        if (blk < full_blocks)
            b = msg + blk * 64;
        else
            b = buf + (blk - full_blocks) * 64;
        uint32_t w[64];
        for (int i = 0; i < 16; i++) {
            w[i] = ((uint32_t)b[i*4]<<24)|((uint32_t)b[i*4+1]<<16)|
                   ((uint32_t)b[i*4+2]<<8)|(uint32_t)b[i*4+3];
        }
        for (int i = 16; i < 64; i++)
            w[i] = G1(w[i-2]) + w[i-7] + G0(w[i-15]) + w[i-16];
        uint32_t a=h[0],b2=h[1],c=h[2],d=h[3],
                 e=h[4],f=h[5],g=h[6],hh=h[7];
        for (int i = 0; i < 64; i++) {
            uint32_t T1 = hh + S1(e) + CH(e,f,g) + sha256_k[i] + w[i];
            uint32_t T2 = S0(a) + MAJ(a,b2,c);
            hh=g; g=f; f=e; e=d+T1;
            d=c; c=b2; b2=a; a=T1+T2;
        }
        h[0]+=a; h[1]+=b2; h[2]+=c; h[3]+=d;
        h[4]+=e; h[5]+=f;  h[6]+=g; h[7]+=hh;
    }
    for (int i = 0; i < 8; i++) {
        digest[i*4+0] = (uint8_t)(h[i]>>24);
        digest[i*4+1] = (uint8_t)(h[i]>>16);
        digest[i*4+2] = (uint8_t)(h[i]>> 8);
        digest[i*4+3] = (uint8_t)(h[i]    );
    }
}

/* ------------------------------------------------------------------ */
/* Monotonic clock (style-guide §11)                                    */
/* ------------------------------------------------------------------ */

static uint64_t mono_ns(void *ctx) {
    (void)ctx;
#if defined(__linux__)
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
#elif defined(__APPLE__)
    return (uint64_t)clock_gettime_nsec_np(CLOCK_MONOTONIC);
#else
#  error "Unsupported platform — extend per style-guide §11"
#endif
}

/* ------------------------------------------------------------------ */
/* RNG — /dev/urandom reads                                             */
/* ------------------------------------------------------------------ */

static int urandom_rng(void *ctx, uint8_t *buf, size_t len) {
    (void)ctx;
    int fd = open("/dev/urandom", O_RDONLY);
    if (fd < 0) return -1;
    size_t total = 0;
    while (total < len) {
        ssize_t r = read(fd, buf + total, len - total);
        if (r < 0) {
            if (errno == EINTR) continue;  /* interrupted — retry */
            close(fd);
            return -1;
        }
        if (r == 0) { close(fd); return -1; }  /* EOF on /dev/urandom — should not happen */
        total += (size_t)r;
    }
    close(fd);
    return 0;
}

/* ------------------------------------------------------------------ */
/* Log sink — no-op                                                     */
/* ------------------------------------------------------------------ */

static void noop_log(void *ctx, int lvl, const char *fmt, ...) {
    (void)ctx; (void)lvl; (void)fmt;
}

/* ------------------------------------------------------------------ */
/* Callbacks struct (AC#3, AC#6; PR2/D2 erratum: struct_size first)     */
/* ------------------------------------------------------------------ */

static t3_callbacks_t g_cbs = {
    /* struct_size is patched at runtime in setup_test_callbacks() so
     * the static zero-initialiser does not violate the _Static_assert
     * check (offsetof guard) before sizeof is known. */
    /* .struct_size  = 0,  -- patched below */
    .lower_send = NULL,  /* parsers under fuzz never call transport */
    .lower_recv = NULL,
    .frame_send = NULL,
    .frame_recv = NULL,
    .rng        = urandom_rng,
    .monotonic_ns = mono_ns,
    .log_sink   = noop_log,
    .ctx        = NULL,
};

const t3_callbacks_t *setup_test_callbacks(void) {
    /* PR2/D2 erratum: MUST set struct_size so the lib accepts the struct. */
    g_cbs.struct_size = sizeof(t3_callbacks_t);
    return &g_cbs;
}

/* ------------------------------------------------------------------ */
/* Side-channel log                                                     */
/* ------------------------------------------------------------------ */

static FILE *g_log_fp = NULL;

void sc_open_log(void) {
    if (g_log_fp) return;  /* idempotent */
    char path[256];
    snprintf(path, sizeof(path), "lib/fuzz/side-channel-%d.log", (int)getpid());
    g_log_fp = fopen(path, "a");
    if (!g_log_fp) {
        /* Try relative path from build dir */
        snprintf(path, sizeof(path), "side-channel-%d.log", (int)getpid());
        g_log_fp = fopen(path, "a");
    }
    if (g_log_fp) {
        /* Line-buffered for minimal latency on emit. */
        setvbuf(g_log_fp, NULL, _IOLBF, 0);
    }
}

void sc_emit(size_t input_len, const uint8_t *data,
             uint64_t parse_ns, uint64_t total_ns) {
    if (!g_log_fp) return;

    /* SHA-256 of input for the log field (truncated to 64 hex chars = 32 bytes). */
    uint8_t digest[32];
    if (data && input_len > 0) {
        sha256_hash(data, input_len, digest);
    } else {
        memset(digest, 0, sizeof(digest));
    }

    char hex[65];
    for (int i = 0; i < 32; i++)
        snprintf(hex + i*2, 3, "%02x", digest[i]);
    hex[64] = '\0';

    fprintf(g_log_fp, "%zu\t%s\t%llu\t%llu\t%d\n",
            input_len,
            hex,
            (unsigned long long)parse_ns,
            (unsigned long long)total_ns,
            (int)getpid());
}
