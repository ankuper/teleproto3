/*
 * test_tgnet_pattern.c — Reproduce the exact tgnet integration pattern:
 *   1. t3_client_create(endpoint, key, dc_id)
 *   2. t3_client_get_fd() → register in poll/kqueue
 *   3. Wait for events → call t3_client_pump()
 *   4. Once READY → t3_client_write/read
 *
 * This catches bugs like:
 *   - EPOLLET edge-trigger missing first event (fd already ready)
 *   - fd changing after handshake
 *   - pump() not progressing state machine
 *
 * Usage:
 *   ./test_tgnet_pattern <endpoint_url> <hex_key>
 *   ./test_tgnet_pattern https://arctic-breeze.my.id:443/ws/7f34ba 78ef151a20066770db00a2f905c103e9
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <poll.h>
#include <time.h>
#include <errno.h>

#include "t3_client.h"

static int hex2bin(const char *hex, uint8_t *out, size_t out_len) {
    size_t hex_len = strlen(hex);
    if (hex_len != out_len * 2) return -1;
    for (size_t i = 0; i < out_len; i++) {
        unsigned int val;
        if (sscanf(hex + i * 2, "%2x", &val) != 1) return -1;
        out[i] = (uint8_t)val;
    }
    return 0;
}

static const char *state_name(t3_client_state_t st) {
    switch (st) {
        case T3_CLIENT_STATE_CONNECTING: return "CONNECTING";
        case T3_CLIENT_STATE_TLS:        return "TLS";
        case T3_CLIENT_STATE_HANDSHAKE:  return "HANDSHAKE";
        case T3_CLIENT_STATE_READY:      return "READY";
        case T3_CLIENT_STATE_ERROR:      return "ERROR";
        case T3_CLIENT_STATE_CLOSED:     return "CLOSED";
        default:                         return "UNKNOWN";
    }
}

int main(int argc, char *argv[]) {
    if (argc < 3) {
        fprintf(stderr, "Usage: %s <endpoint_url> <hex_key_32chars>\n", argv[0]);
        fprintf(stderr, "  e.g.: %s https://arctic-breeze.my.id:443/ws/7f34ba 78ef151a20066770db00a2f905c103e9\n", argv[0]);
        return 1;
    }

    const char *url = argv[1];
    const char *hex_key = argv[2];
    uint8_t key[16];

    if (hex2bin(hex_key, key, 16) != 0) {
        fprintf(stderr, "ERROR: key must be 32 hex chars\n");
        return 1;
    }

    printf("=== test_tgnet_pattern ===\n");
    printf("endpoint: %s\n", url);
    printf("key: %s\n", hex_key);

    /* Step 1: Create stream (like tgnet openConnection) */
    printf("\n[1] t3_client_create...\n");
    t3_client_stream *stream = NULL;
    int16_t dc_id = argc >= 4 ? (int16_t)atoi(argv[3]) : 2;
    printf("dc_id: %d\n", dc_id);
    t3_result_t rc = t3_client_create(url, key, dc_id, &stream);
    printf("    rc=%d stream=%p\n", rc, (void*)stream);
    if (rc != T3_OK || !stream) {
        fprintf(stderr, "FAIL: t3_client_create returned %d\n", rc);
        return 2;
    }

    /* Step 2: Get fd (like tgnet does for epoll) */
    int fd = t3_client_get_fd(stream);
    printf("[2] fd=%d\n", fd);
    if (fd < 0) {
        fprintf(stderr, "FAIL: get_fd returned %d\n", fd);
        t3_client_destroy(stream);
        return 3;
    }

    t3_client_state_t state = t3_client_get_state(stream);
    printf("    initial state: %s\n", state_name(state));

    /* Step 3: Check if fd is ALREADY ready (the EPOLLET bug) */
    printf("\n[3] Checking if fd is already ready (poll timeout=0)...\n");
    struct pollfd pfd = { .fd = fd, .events = POLLIN | POLLOUT };
    int poll_rc = poll(&pfd, 1, 0);
    printf("    poll(timeout=0): rc=%d revents=0x%x", poll_rc, pfd.revents);
    if (pfd.revents & POLLOUT) printf(" POLLOUT");
    if (pfd.revents & POLLIN) printf(" POLLIN");
    if (pfd.revents & POLLERR) printf(" POLLERR");
    if (pfd.revents & POLLHUP) printf(" POLLHUP");
    printf("\n");

    if (poll_rc > 0) {
        printf("    *** fd IS already ready at registration time!\n");
        printf("    *** EPOLLET would miss this. Must call pump() immediately.\n");
    } else {
        printf("    fd not ready yet — EPOLLET would work normally.\n");
    }

    /* Step 4: Pump loop — drive handshake to READY (like tgnet onEvent) */
    printf("\n[4] Pump loop (max 15s)...\n");
    time_t start = time(NULL);
    int pump_count = 0;
    t3_client_state_t prev_state = state;

    /* CRITICAL: call pump() once immediately — the EPOLLET fix */
    rc = t3_client_pump(stream);
    pump_count++;
    state = t3_client_get_state(stream);
    printf("    pump[%d] (immediate): rc=%d state=%s\n", pump_count, rc, state_name(state));

    while (state != T3_CLIENT_STATE_READY && state != T3_CLIENT_STATE_ERROR) {
        if (time(NULL) - start > 15) {
            fprintf(stderr, "TIMEOUT: stuck in %s after 15s\n", state_name(state));
            t3_client_destroy(stream);
            return 4;
        }

        pfd.fd = fd;
        pfd.events = POLLIN | POLLOUT;
        pfd.revents = 0;
        int pr = poll(&pfd, 1, 500);

        if (pr > 0) {
            rc = t3_client_pump(stream);
            pump_count++;
            state = t3_client_get_state(stream);
            if (state != prev_state) {
                printf("    pump[%d]: state %s → %s (rc=%d)\n",
                       pump_count, state_name(prev_state), state_name(state), rc);
                prev_state = state;
            }

            /* Check if fd changed (hypothesis: TLS wraps the socket) */
            int new_fd = t3_client_get_fd(stream);
            if (new_fd != fd) {
                printf("    *** fd CHANGED: %d → %d\n", fd, new_fd);
                fd = new_fd;
            }
        }
    }

    if (state == T3_CLIENT_STATE_ERROR) {
        fprintf(stderr, "FAIL: stream error: %s\n", t3_client_last_error(stream));
        t3_client_destroy(stream);
        return 5;
    }

    printf("    READY after %d pumps, %lds\n", pump_count, time(NULL) - start);

    /* Step 5: Write req_pq_multi — RAW MTProto, library adds intermediate length
       MTProto unencrypted: auth_key_id(8)=0 + msg_id(8) + msg_len(4) + req_pq_multi(4) + nonce(16) = 36 bytes */
    printf("\n[5] Write req_pq_multi (raw MTProto, lib adds framing)...\n");
    uint8_t req_pq[36];
    /* auth_key_id = 0 (unencrypted) */
    memset(req_pq, 0, 8);
    /* msg_id = some unique value (8 bytes, must be divisible by 4) */
    uint64_t msg_id = ((uint64_t)time(NULL)) << 32 | 4;
    memcpy(req_pq + 8, &msg_id, 8);
    /* msg_len = 20 (req_pq_multi TL constructor + 16-byte nonce) */
    uint32_t msg_len = 20;
    memcpy(req_pq + 16, &msg_len, 4);
    /* TL constructor: req_pq_multi = 0xbe7e8ef1 */
    uint32_t constructor = 0xbe7e8ef1;
    memcpy(req_pq + 20, &constructor, 4);
    /* 16-byte random nonce */
    for (int i = 0; i < 16; i++) req_pq[24 + i] = (uint8_t)(i + 1);

    rc = t3_client_write(stream, req_pq, sizeof(req_pq));
    printf("    write rc=%d (%zu bytes)\n", rc, sizeof(req_pq));
    /* Pump to flush SSL write buffer */
    t3_client_pump(stream);

    /* Step 6: Try to read response */
    printf("\n[6] Read response (5s timeout)...\n");
    uint8_t read_buf[4096];
    size_t read_len = 0;
    time_t read_start = time(NULL);

    while (time(NULL) - read_start < 5) {
        pfd.fd = fd;
        pfd.events = POLLIN | POLLOUT;
        pfd.revents = 0;
        if (poll(&pfd, 1, 100) >= 0) {
            t3_client_pump(stream); /* flush writes + read incoming */
            rc = t3_client_read(stream, read_buf, sizeof(read_buf), &read_len);
            if (rc == T3_OK && read_len > 0) {
                printf("    read %zu bytes, rc=%d\n", read_len, rc);
                printf("    first 8 bytes:");
                for (size_t i = 0; i < (read_len < 8 ? read_len : 8); i++)
                    printf(" %02x", read_buf[i]);
                printf("\n");
                break;
            }
        }
    }

    if (read_len == 0) {
        printf("    no response (ok for Type3 — server may need valid MTProto)\n");
    }

    /* Cleanup */
    printf("\n[7] Cleanup...\n");
    t3_client_destroy(stream);
    printf("    done.\n");

    printf("\n=== PASS ===\n");
    return 0;
}
