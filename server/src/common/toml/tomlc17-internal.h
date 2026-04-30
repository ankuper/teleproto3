/* Copyright (c) 2024-2026, CK Tan.
 * https://github.com/cktan/tomlc17/blob/main/LICENSE
 */

#pragma once

#include "tomlc17.h"
#include <assert.h>
#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <math.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern const toml_datum_t DATUM_ZERO;
extern toml_option_t toml_option;

#define MALLOC(n) toml_option.mem_realloc(0, n)
#define REALLOC(p, n) toml_option.mem_realloc(p, n)
#define FREE(p) toml_option.mem_free(p)

#define DO(x)                                                                  \
  if (x)                                                                       \
    return -1;                                                                 \
  else                                                                         \
    (void)0

static inline int copystring(char *dst, int dstsz, const char *src) {
  int srcsz = strlen(src) + 1;
  if (srcsz > dstsz) {
    return -1;
  }
  memcpy(dst, src, srcsz);
  return 0;
}

/*
 *  Error buffer
 */
typedef struct ebuf_t ebuf_t;
struct ebuf_t {
  char *ptr;
  int len;
};

int SETERROR(ebuf_t ebuf, int lineno, const char *fmt, ...);

/* This is a string view. */
typedef struct span_t span_t;
struct span_t {
  const char *ptr;
  int len;
};

// flags for toml_datum_t::flag.
#define FLAG_INLINED 1
#define FLAG_STDEXPR 2
#define FLAG_EXPLICIT 4

// Maximum levels of brackets and braces
#define BRACKET_LEVEL_MAX 30
#define BRACE_LEVEL_MAX 30

static inline size_t align8(size_t x) { return (((x) + 7) & ~7); }

enum toktyp_t {
  TOK_DOT = 1,
  TOK_EQUAL,
  TOK_COMMA,
  TOK_LBRACK,
  TOK_LLBRACK,
  TOK_RBRACK,
  TOK_RRBRACK,
  TOK_LBRACE,
  TOK_RBRACE,
  TOK_LIT,
  TOK_STRING,
  TOK_MLSTRING,
  TOK_LITSTRING,
  TOK_MLLITSTRING,
  TOK_TIME,
  TOK_DATE,
  TOK_DATETIME,
  TOK_DATETIMETZ,
  TOK_INTEGER,
  TOK_FLOAT,
  TOK_BOOL,
  TOK_ENDL,
  TOK_FIN = -5000, // EOF
};
typedef enum toktyp_t toktyp_t;
typedef struct scanner_t scanner_t;

/* Remember the current state of a scanner */
typedef struct scanner_state_t scanner_state_t;
struct scanner_state_t {
  scanner_t *sp;
  const char *cur; // points into scanner_t::src[]
  int lineno;      // current line number
};

// A scan token
typedef struct token_t token_t;
struct token_t {
  toktyp_t toktyp;
  int lineno;
  span_t str;

  // values represented by str
  union {
    const char *escp; // point to an esc char in str
    int64_t int64;
    double fp64;
    bool b1;
    struct {
      // validity depends on toktyp for TIME, DATE, DATETIME, DATETIMETZ
      int year, month, day, hour, minute, sec, usec;
      int tz; // +- minutes
    } tsval;
  } u;
};

// Scanner object
struct scanner_t {
  const char *src;  // src[] is a NUL-terminated string
  const char *endp; // end of src[]. always pointing at a NUL char.
  const char *cur;  // current char in src[]
  int lineno;       // line number of current char
  char *errmsg;     // point to errbuf if there was an error
  ebuf_t ebuf;

  int bracket_level; // count depth of [ ]
  int brace_level;   // count depth of { }
};

#ifndef min
static inline int min(int a, int b) { return a < b ? a : b; }
#endif

static inline void scan_copystr(scanner_t *sp, char *dst, int dstsz) {
  assert(dstsz > 0);
  int len = min(sp->endp - sp->cur, dstsz - 1);
  if (len > 0) {
    memcpy(dst, sp->cur, len);
    dst[len] = 0;
  }
}

/* Scanner functions (defined in tomlc17-scan.c) */
void scan_init(scanner_t *sp, const char *src, int len, char *errbuf,
               int errbufsz);
int scan_key(scanner_t *sp, token_t *tok);
int scan_value(scanner_t *sp, token_t *tok);
scanner_state_t scan_mark(scanner_t *sp);
void scan_restore(scanner_t *sp, scanner_state_t state);
int check_overflow(scanner_t *sp, token_t *tok);

/* UTF-8 functions (defined in tomlc17-scan.c) */
int utf8_to_ucs(const char *s, int len, uint32_t *ret);
int ucs_to_utf8(uint32_t code, char buf[6]);
