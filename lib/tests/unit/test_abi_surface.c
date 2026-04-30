/*
 * test_abi_surface.c — ABI surface acceptance test (story 1.7 AC6, AC7).
 *
 * Not subject to banner-discipline (tests/, not src/).
 * Source: story 1.7 AC6 + AC7 + docs/epic-1-style-guide.md §9, §10.
 * Returns 0 on pass / 1 on fail.
 *
 * Uses `nm -g --defined-only` which works on both static archives (.a)
 * and shared objects (.so/.dylib). On macOS, symbol names have a leading
 * underscore which is stripped before comparison.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* Expected public symbol list — style-guide §9 verbatim              */
/* ------------------------------------------------------------------ */

static const char *EXPECTED_SYMBOLS[] = {
    /* Functions: */
    "t3_secret_parse",
    "t3_secret_free",
    "t3_secret_serialise",
    "t3_secret_validate_host",
    "t3_secret_validate_path",
    "t3_secret_zeroise",
    "t3_session_new",
    "t3_session_free",
    "t3_header_parse",
    "t3_header_serialise",
    "t3_session_bind_callbacks",
    "t3_session_negotiate_version",
    "t3_silent_close_delay_sample_ns",
    "t3_retry_record_close",
    "t3_retry_get_state",
    "t3_retry_user_retry",
    "t3_strerror",
    "t3_abi_version_string",
    NULL
};

/* Internal symbols that are expected in the archive but NOT part of
 * the public ABI. These are hidden-visibility helpers needed across
 * translation units within the library. */
static const char *KNOWN_INTERNAL[] = {
    "t3_retry_ring_record",
    "t3_retry_ring_get",
    "t3_retry_ring_user_retry",
    "t3_session_handle_header_byte",
    "t3_timing_rejection_sample_uniform_ns",
    "t3_url_parse",
    NULL
};

/* ------------------------------------------------------------------ */
/* nm-audit forbidden namespace regex (style-guide §10)                */
/* 1.7-UNIT-025                                                        */
/* ------------------------------------------------------------------ */

static const char *FORBIDDEN_SUBSTRINGS[] = {
    "ssl", "tls", "openssl", "boringssl", "mbedtls", "gnutls", "wolfssl",
    "schannel", "securetransport", "cf_", "cfnetwork", "winhttp", "bcrypt",
    "secitem", "qt", "qwebsocket", "nw_", "nwwebsocket", "conscrypt",
    "okhttp", "websocket",
    NULL
};

static int ci_substr(const char *haystack, const char *needle) {
    size_t nlen = strlen(needle);
    size_t hlen = strlen(haystack);
    if (nlen > hlen) return 0;
    for (size_t i = 0; i <= hlen - nlen; i++) {
        int match = 1;
        for (size_t j = 0; j < nlen; j++) {
            char hc = haystack[i+j];
            char nc = needle[j];
            if (hc >= 'A' && hc <= 'Z') hc += 32;
            if (nc >= 'A' && nc <= 'Z') nc += 32;
            if (hc != nc) { match = 0; break; }
        }
        if (match) return 1;
    }
    return 0;
}

static int is_known(const char *sym, const char **list) {
    for (int i = 0; list[i]; i++)
        if (strcmp(sym, list[i]) == 0) return 1;
    return 0;
}

/* ------------------------------------------------------------------ */
/* Run nm and parse output                                             */
/* ------------------------------------------------------------------ */

static int run_abi_surface_check(const char *lib_path) {
    char cmd[1024];
    /* nm -g: global symbols only; --defined-only: skip undefined refs.
     * Works on both .a (static) and .so/.dylib (shared). */
    snprintf(cmd, sizeof cmd, "nm -g --defined-only %s 2>/dev/null", lib_path);
    FILE *pipe = popen(cmd, "r");
    if (!pipe) {
        fprintf(stderr, "FAIL [nm-open]: cannot popen nm for %s\n", lib_path);
        return 1;
    }

    /* Collect all t3_* symbols. */
    char line[512];
    char found[128][128];
    int found_count = 0;
    while (fgets(line, sizeof line, pipe)) {
        /* nm output: "address type name" */
        char *sym = NULL;
        char type_char = ' ';
        char *tok = strtok(line, " \t\n");
        while (tok) {
            /* Track the second-to-last and last tokens. */
            if (strlen(tok) == 1 && tok[0] >= 'A' && tok[0] <= 'z')
                type_char = tok[0];
            sym = tok;
            tok = strtok(NULL, " \t\n");
        }
        if (!sym) continue;
        /* Only inspect text (code) symbols — type 'T' or 't'. */
        if (type_char != 'T' && type_char != 't') continue;
        /* macOS prefixes symbols with '_' — strip it. */
        if (sym[0] == '_') sym++;
        /* Only inspect t3_* symbols. */
        if (strncmp(sym, "t3_", 3) != 0) continue;
        if (found_count < 128) {
            strncpy(found[found_count], sym, 127);
            found[found_count][127] = '\0';
            found_count++;
        }
    }
    pclose(pipe);

    int failures = 0;

    /* AC6: Every expected symbol must be present. */
    for (int i = 0; EXPECTED_SYMBOLS[i]; i++) {
        int ok = 0;
        for (int j = 0; j < found_count; j++) {
            if (strcmp(found[j], EXPECTED_SYMBOLS[i]) == 0) { ok = 1; break; }
        }
        if (!ok) {
            fprintf(stderr, "FAIL [1.7-UNIT-024]: missing ABI symbol: %s\n", EXPECTED_SYMBOLS[i]);
            failures++;
        }
    }

    /* AC6: No unexpected t3_ symbols beyond ABI + known-internal. */
    for (int j = 0; j < found_count; j++) {
        if (is_known(found[j], EXPECTED_SYMBOLS)) continue;
        if (is_known(found[j], KNOWN_INTERNAL)) continue;
        fprintf(stderr, "FAIL [1.7-UNIT-024]: unexpected symbol: %s\n", found[j]);
        failures++;
    }

    if (failures == 0)
        printf("PASS [1.7-UNIT-024]: ABI surface matches §9 enumeration (%d public + %d known-internal)\n",
               found_count - 0, 0); /* counts are informational */

    /* AC7: No forbidden namespace matches. */
    snprintf(cmd, sizeof cmd, "nm -a %s 2>/dev/null", lib_path);
    pipe = popen(cmd, "r");
    if (pipe) {
        int nm_failures = 0;
        while (fgets(line, sizeof line, pipe)) {
            char *sym = NULL;
            char *tok = strtok(line, " \t\n");
            while (tok) { sym = tok; tok = strtok(NULL, " \t\n"); }
            if (!sym) continue;
            if (sym[0] == '_') sym++;
            for (int k = 0; FORBIDDEN_SUBSTRINGS[k]; k++) {
                if (ci_substr(sym, FORBIDDEN_SUBSTRINGS[k])) {
                    fprintf(stderr, "FAIL [1.7-UNIT-025]: forbidden namespace match: sym=%s pattern=%s\n",
                            sym, FORBIDDEN_SUBSTRINGS[k]);
                    nm_failures++;
                    failures++;
                }
            }
        }
        pclose(pipe);
        if (nm_failures == 0)
            printf("PASS [1.7-UNIT-025]: nm-audit — 0 forbidden namespace matches\n");
    }

    /* AC9: no ssize_t in public headers — check t3.h. */
    {
        int ssize_fail = system("grep -rn 'ssize_t' ../../include/t3.h >/dev/null 2>&1");
        if (ssize_fail == 0) {
            fprintf(stderr, "FAIL [1.7-UNIT-029]: ssize_t found in public header t3.h\n");
            failures++;
        } else {
            printf("PASS [1.7-UNIT-029]: no ssize_t in t3.h\n");
        }
    }

    return failures > 0 ? 1 : 0;
}

int main(int argc, char **argv) {
    printf("=== test_abi_surface: ABI acceptance test ===\n");
    const char *lib_path = argc > 1 ? argv[1] : "libteleproto3.a";
    return run_abi_surface_check(lib_path);
}
