/*
 * socks5_conformance_stub.c — helper binary for socks5_conformance_test.py.
 *
 * Reads configuration from environment variables, opens a t3_shim, prints
 * "READY" on stdout, then blocks until signal. The pytest harness starts
 * this process, reads "READY", runs compliance tests against the shim, then
 * terminates the process.
 *
 * Environment:
 *   T3_SHIM_STUB_SERVER  "host:port" of the mock Type3 server
 *   T3_SHIM_STUB_SECRET  full Type3 secret hex string (ff + 32 hex + domain)
 *   T3_SHIM_STUB_PORT    desired local SOCKS5 port (0 = ephemeral)
 *   T3_SHIM_STUB_CA      path to CA cert PEM for TLS verification
 *
 * Built only when T3_SHIM_SOCKS5=ON (same guard as the shim itself).
 */

#include "t3_shim_socks5.h"
#include <openssl/ssl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static volatile int g_running = 1;
static void on_sig(int s) { (void)s; g_running = 0; }

int main(void) {
    signal(SIGTERM, on_sig);
    signal(SIGINT,  on_sig);

    const char *srv = getenv("T3_SHIM_STUB_SERVER");
    const char *sec = getenv("T3_SHIM_STUB_SECRET");
    const char *pstr = getenv("T3_SHIM_STUB_PORT");
    const char *ca   = getenv("T3_SHIM_STUB_CA");
    if (!srv || !sec) {
        fprintf(stderr, "stub: T3_SHIM_STUB_SERVER and T3_SHIM_STUB_SECRET required\n");
        return 1;
    }

    /* Parse host:port */
    char host[256];
    uint16_t port = 443;
    const char *colon = strrchr(srv, ':');
    if (colon) {
        size_t hlen = (size_t)(colon - srv);
        if (hlen >= sizeof(host)) hlen = sizeof(host) - 1;
        memcpy(host, srv, hlen);
        host[hlen] = 0;
        port = (uint16_t)atoi(colon + 1);
    } else {
        strncpy(host, srv, sizeof(host) - 1);
    }

    uint16_t local_port = pstr ? (uint16_t)atoi(pstr) : 0;

    /* Override CA cert for test TLS verification */
    if (ca) {
        /* OpenSSL: set SSL_CERT_FILE so default_verify_paths picks it up */
        setenv("SSL_CERT_FILE", ca, 1);
    }

    t3_shim_t *shim = NULL;
    t3_result_t rc = t3_shim_open(host, port, "/ws/test", sec, local_port, &shim);
    if (rc != T3_OK) {
        fprintf(stderr, "stub: t3_shim_open failed: %s\n", t3_strerror(rc));
        return 1;
    }

    /* D6: expose the shim's auto-generated SOCKS5 USER/PASS so the test
     * harness can authenticate against the loopback listener. NEVER do
     * this in production — the stub is a test-only fixture. */
    char user[T3_SHIM_CRED_BUFLEN] = {0};
    char pass[T3_SHIM_CRED_BUFLEN] = {0};
    if (t3_shim_get_credentials(shim, user, sizeof(user), pass, sizeof(pass)) != T3_OK) {
        fprintf(stderr, "stub: t3_shim_get_credentials failed\n");
        t3_shim_close(shim);
        return 1;
    }

    printf("READY\n");
    fflush(stdout);
    printf("PORT %u\n", (unsigned)t3_shim_local_port(shim));
    fflush(stdout);
    printf("USER %s\n", user);
    fflush(stdout);
    printf("PASS %s\n", pass);
    fflush(stdout);

    while (g_running) sleep(1);

    t3_shim_close(shim);
    return 0;
}
