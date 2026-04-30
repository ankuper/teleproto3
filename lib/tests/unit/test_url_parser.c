/*
 * test_url_parser.c — unit tests for t3_url_parse.
 * Not subject to banner-discipline (tests/, not src/).
 * Source: story 1.7 Task 8.
 */

#include "t3.h"
#include <stdio.h>
#include <string.h>

/* t3_url_parse is internal — forward declare. */
t3_result_t t3_url_parse(const char *url, size_t len, t3_secret_t **out);

int main(int argc, char **argv) {
    (void)argc; (void)argv;
    int rc = 0;

    /* NULL inputs. */
    t3_secret_t *s = NULL;
    if (t3_url_parse(NULL, 0, &s) != T3_ERR_INVALID_ARG) {
        fprintf(stderr, "[url] FAIL: null url\n"); rc = 1;
    }
    if (t3_url_parse("", 0, NULL) != T3_ERR_INVALID_ARG) {
        fprintf(stderr, "[url] FAIL: null out\n"); rc = 1;
    }

    /* Wrong scheme. */
    if (t3_url_parse("http://example.com", 18, &s) != T3_ERR_MALFORMED) {
        fprintf(stderr, "[url] FAIL: wrong scheme\n"); rc = 1;
    }

    /* No secret param. */
    if (t3_url_parse("tg://proxy?server=example.com&port=443", 38, &s) != T3_ERR_MALFORMED) {
        fprintf(stderr, "[url] FAIL: no secret\n"); rc = 1;
    }

    /* Valid: 18-byte secret = 0xff + 16 zero key + 'x' = 36 hex chars. */
    /* "ff" + 32 zeros + "78" ('x') */
    const char *valid_url = "tg://proxy?server=example.com&port=443&secret=ff0000000000000000000000000000000078";
    if (t3_url_parse(valid_url, strlen(valid_url), &s) != T3_OK || !s) {
        fprintf(stderr, "[url] FAIL: valid url parse\n"); rc = 1;
    } else {
        t3_secret_free(s);
    }

    if (!rc) printf("[url] PASS all url_parser tests\n");
    return rc;
}
