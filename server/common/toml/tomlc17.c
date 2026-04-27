/* Copyright (c) 2024-2026, CK Tan.
 * https://github.com/cktan/tomlc17/blob/main/LICENSE
 */
#include "tomlc17-internal.h"

const toml_datum_t DATUM_ZERO = {0};
toml_option_t toml_option = {0, realloc, free};

/*
 *  Format an error into ebuf[]. Always return -1.
 */
int SETERROR(ebuf_t ebuf, int lineno, const char *fmt, ...) {
  va_list args;
  va_start(args, fmt);
  char *p = ebuf.ptr;
  char *q = p + ebuf.len;
  if (lineno) {
    snprintf(p, p < q ? q - p : 0, "(line %d) ", lineno);
    p += strlen(p);
  }
  vsnprintf(p, p < q ? q - p : 0, fmt, args);
  va_end(args);
  return -1;
}

/*
 *  Memory pool. Allocated a big block once and hand out piecemeal.
 */
typedef struct pool_t pool_t;
struct pool_t {
  int max;     // size of buf[]
  int top;     // offset of first free byte in buf[]
  char buf[1]; // first byte starts here
};

/**
 *  Create a memory pool of N bytes. Return the memory pool on
 *  success, or NULL if out of memory.
 */
static pool_t *pool_create(int N) {
  if (N <= 0) {
    N = 100; // minimum
  }
  int totalsz = sizeof(pool_t) + N;
  pool_t *pool = MALLOC(totalsz);
  if (!pool) {
    return NULL;
  }
  memset(pool, 0, totalsz);
  pool->max = N;
  return pool;
}

/**
 *  Destroy a memory pool.
 */
static void pool_destroy(pool_t *pool) { FREE(pool); }

/**
 *  Allocate n bytes from pool. Return the memory allocated on
 *  success, or NULL if out of memory.
 */
static char *pool_alloc(pool_t *pool, int n) {
  if (pool->top + n > pool->max) {
    return NULL;
  }
  char *ret = pool->buf + pool->top;
  pool->top += n;
  return ret;
}

/* Represents a multi-part key */
#define KEYPARTMAX 10
typedef struct keypart_t keypart_t;
struct keypart_t {
  int nspan;
  span_t span[KEYPARTMAX];
};

// Parser object
typedef struct parser_t parser_t;
struct parser_t {
  scanner_t scanner;
  toml_datum_t toptab;  // top table
  toml_datum_t *curtab; // current table
  pool_t *pool;         // memory pool for strings
  ebuf_t ebuf;
};

// Put key into tab dictionary. Return a place to
// the datum for the key on success, or NULL otherwise.
static toml_datum_t *tab_emplace(toml_datum_t *tab, span_t key,
                                 const char **reason) {
  assert(tab->type == TOML_TABLE);
  int N = tab->u.tab.size;
  for (int i = 0; i < N; i++) {
    if (tab->u.tab.len[i] == key.len &&
        0 == memcmp(tab->u.tab.key[i], key.ptr, key.len)) {
      return &tab->u.tab.value[i];
    }
  }
  // Expand pkey[], plen[] and value[]. The following does this
  // separately for pkey, plen and value, and it is safe on partial
  // failure, i.e. only the first one succeeded.
  {
    char **pkey = REALLOC(tab->u.tab.key, sizeof(*pkey) * align8(N + 1));
    if (!pkey) {
      *reason = "out of memory";
      return NULL;
    }
    tab->u.tab.key = (const char **)pkey;
  }

  {
    int *plen = REALLOC(tab->u.tab.len, sizeof(*plen) * align8(N + 1));
    if (!plen) {
      *reason = "out of memory";
      return NULL;
    }
    tab->u.tab.len = plen;
  }

  {
    toml_datum_t *value =
        REALLOC(tab->u.tab.value, sizeof(*value) * align8(N + 1));
    if (!value) {
      *reason = "out of memory";
      return NULL;
    }
    tab->u.tab.value = value;
  }

  // Append the new key/value
  tab->u.tab.size = N + 1;
  tab->u.tab.key[N] = (char *)key.ptr;
  tab->u.tab.len[N] = key.len;
  tab->u.tab.value[N] = DATUM_ZERO;
  return &tab->u.tab.value[N];
}

// Find key in tab and return its index. If not found, return -1.
static int tab_find(toml_datum_t *tab, span_t key) {
  assert(tab->type == TOML_TABLE);
  for (int i = 0, top = tab->u.tab.size; i < top; i++) {
    if (tab->u.tab.len[i] == key.len &&
        0 == memcmp(tab->u.tab.key[i], key.ptr, key.len)) {
      return i;
    }
  }
  return -1;
}

// Add a new key in tab. Return 0 on success, -1 otherwise.
// On error, *reason will point to an error message.
static int tab_add(toml_datum_t *tab, span_t newkey, toml_datum_t newvalue,
                   const char **reason) {
  assert(tab->type == TOML_TABLE);
  if (-1 != tab_find(tab, newkey)) {
    *reason = "duplicate key";
    return -1;
  }
  toml_datum_t *pvalue = tab_emplace(tab, newkey, reason);
  if (!pvalue) {
    return -1;
  }
  *pvalue = newvalue;
  return 0;
}

// Add a new element into an array. Return 0 on success, -1 otherwise.
// On error, *reason will point to an error message.
static toml_datum_t *arr_emplace(toml_datum_t *arr, const char **reason) {
  assert(arr->type == TOML_ARRAY);
  int n = arr->u.arr.size;
  toml_datum_t *elem = REALLOC(arr->u.arr.elem, sizeof(*elem) * align8(n + 1));
  if (!elem) {
    *reason = "out of memory";
    return NULL;
  }
  arr->u.arr.elem = elem;
  arr->u.arr.size = n + 1;
  elem[n] = DATUM_ZERO;
  return &elem[n];
}

// ------------------- parser section
static int parse_norm(parser_t *pp, token_t tok, span_t *ret_span);
static int parse_val(parser_t *pp, token_t tok, toml_datum_t *ret);
static int parse_keyvalue_expr(parser_t *pp, token_t tok);
static int parse_std_table_expr(parser_t *pp, token_t tok);
static int parse_array_table_expr(parser_t *pp, token_t tok);

static toml_datum_t mkdatum(toml_type_t ty) {
  toml_datum_t ret = {0};
  ret.type = ty;
  if (ty == TOML_DATE || ty == TOML_TIME || ty == TOML_DATETIME ||
      ty == TOML_DATETIMETZ) {
    ret.u.ts.year = -1;
    ret.u.ts.month = -1;
    ret.u.ts.day = -1;
    ret.u.ts.hour = -1;
    ret.u.ts.minute = -1;
    ret.u.ts.second = -1;
    ret.u.ts.usec = -1;
    ret.u.ts.tz = -1;
  }
  return ret;
}

// Recursively free any dynamically allocated memory in the datum tree
static void datum_free(toml_datum_t *datum) {
  if (datum->type == TOML_TABLE) {
    for (int i = 0, top = datum->u.tab.size; i < top; i++) {
      datum_free(&datum->u.tab.value[i]);
    }
    FREE(datum->u.tab.key);
    FREE(datum->u.tab.len);
    FREE(datum->u.tab.value);
  } else if (datum->type == TOML_ARRAY) {
    for (int i = 0, top = datum->u.arr.size; i < top; i++) {
      datum_free(&datum->u.arr.elem[i]);
    }
    FREE(datum->u.arr.elem);
  }
  // other types do not allocate memory
  *datum = DATUM_ZERO;
}

// Make a deep copy of src to dst.
// Return 0 on success, -1 otherwise.
static int datum_copy(toml_datum_t *dst, toml_datum_t src, pool_t *pool,
                      const char **reason) {
  *dst = mkdatum(src.type);
  switch (src.type) {
  case TOML_STRING:
    dst->u.str.ptr = pool_alloc(pool, src.u.str.len + 1);
    if (!dst->u.str.ptr) {
      *reason = "out of memory";
      goto bail;
    }
    dst->u.str.len = src.u.str.len;
    memcpy((char *)dst->u.str.ptr, src.u.str.ptr, src.u.str.len + 1);
    break;
  case TOML_TABLE:
    for (int i = 0; i < src.u.tab.size; i++) {
      span_t newkey = {src.u.tab.key[i], src.u.tab.len[i]};
      toml_datum_t *pvalue = tab_emplace(dst, newkey, reason);
      if (!pvalue) {
        goto bail;
      }
      if (datum_copy(pvalue, src.u.tab.value[i], pool, reason)) {
        goto bail;
      }
    }
    break;
  case TOML_ARRAY:
    for (int i = 0; i < src.u.arr.size; i++) {
      toml_datum_t *pelem = arr_emplace(dst, reason);
      if (!pelem) {
        goto bail;
      }
      if (datum_copy(pelem, src.u.arr.elem[i], pool, reason)) {
        goto bail;
      }
    }
    break;
  default:
    *dst = src;
    break;
  }

  return 0;

bail:
  datum_free(dst);
  return -1;
}

// Check if datum is an array of tables.
static inline bool is_array_of_tables(toml_datum_t datum) {
  bool ret = (datum.type == TOML_ARRAY);
  for (int i = 0; ret && i < datum.u.arr.size; i++) {
    ret = (datum.u.arr.elem[i].type == TOML_TABLE);
  }
  return ret;
}

// Merge src into dst. Return 0 on success, -1 otherwise.
static int datum_merge(toml_datum_t *dst, toml_datum_t src, pool_t *pool,
                       const char **reason) {
  if (dst->type != src.type) {
    datum_free(dst);
    return datum_copy(dst, src, pool, reason);
  }
  switch (src.type) {
  case TOML_TABLE:
    // for key-value in src:
    //    override key-value in dst.
    for (int i = 0; i < src.u.tab.size; i++) {
      span_t key;
      key.ptr = src.u.tab.key[i];
      key.len = src.u.tab.len[i];
      toml_datum_t *pvalue = tab_emplace(dst, key, reason);
      if (!pvalue) {
        return -1;
      }
      if (pvalue->type) {
        DO(datum_merge(pvalue, src.u.tab.value[i], pool, reason));
      } else {
        datum_free(pvalue);
        DO(datum_copy(pvalue, src.u.tab.value[i], pool, reason));
      }
    }
    return 0;
  case TOML_ARRAY:
    if (is_array_of_tables(src)) {
      // append src array to dst
      for (int i = 0; i < src.u.arr.size; i++) {
        toml_datum_t *pelem = arr_emplace(dst, reason);
        if (!pelem) {
          return -1;
        }
        DO(datum_copy(pelem, src.u.arr.elem[i], pool, reason));
      }
      return 0;
    }
    // fallthru
  default:
    break;
  }
  datum_free(dst);
  return datum_copy(dst, src, pool, reason);
}

// Compare the content of a and b.
static bool datum_equiv(toml_datum_t a, toml_datum_t b) {
  if (a.type != b.type) {
    return false;
  }
  int N;
  switch (a.type) {
  case TOML_STRING:
    return a.u.str.len == b.u.str.len &&
           0 == memcmp(a.u.str.ptr, b.u.str.ptr, a.u.str.len);
  case TOML_INT64:
    return a.u.int64 == b.u.int64;
  case TOML_FP64:
    return a.u.fp64 == b.u.fp64 || (isnan(a.u.fp64) && isnan(b.u.fp64));
  case TOML_BOOLEAN:
    return !!a.u.boolean == !!b.u.boolean;
  case TOML_DATE:
    return a.u.ts.year == b.u.ts.year && a.u.ts.month == b.u.ts.month &&
           a.u.ts.day == b.u.ts.day;
  case TOML_TIME:
    return a.u.ts.hour == b.u.ts.hour && a.u.ts.minute == b.u.ts.minute &&
           a.u.ts.second == b.u.ts.second && a.u.ts.usec == b.u.ts.usec;
  case TOML_DATETIME:
    return a.u.ts.year == b.u.ts.year && a.u.ts.month == b.u.ts.month &&
           a.u.ts.day == b.u.ts.day && a.u.ts.hour == b.u.ts.hour &&
           a.u.ts.minute == b.u.ts.minute && a.u.ts.second == b.u.ts.second &&
           a.u.ts.usec == b.u.ts.usec;
  case TOML_DATETIMETZ:
    return a.u.ts.year == b.u.ts.year && a.u.ts.month == b.u.ts.month &&
           a.u.ts.day == b.u.ts.day && a.u.ts.hour == b.u.ts.hour &&
           a.u.ts.minute == b.u.ts.minute && a.u.ts.second == b.u.ts.second &&
           a.u.ts.usec == b.u.ts.usec && a.u.ts.tz == b.u.ts.tz;
  case TOML_ARRAY:
    N = a.u.arr.size;
    if (N != b.u.arr.size) {
      return false;
    }
    for (int i = 0; i < N; i++) {
      if (!datum_equiv(a.u.arr.elem[i], b.u.arr.elem[i])) {
        return false;
      }
    }
    return true;
  case TOML_TABLE:
    N = a.u.tab.size;
    if (N != b.u.tab.size) {
      return false;
    }
    for (int i = 0; i < N; i++) {
      int len = a.u.tab.len[i];
      if (len != b.u.tab.len[i]) {
        return false;
      }
      if (0 != memcmp(a.u.tab.key[i], b.u.tab.key[i], len)) {
        return false;
      }
      if (!datum_equiv(a.u.tab.value[i], b.u.tab.value[i])) {
        return false;
      }
    }
    return true;
  default:
    break;
  }
  return false;
}

/**
 *  Override values in r1 using r2. Return a new result. All results
 *  (i.e., r1, r2 and the returned result) must be freed using toml_free()
 *  after use.
 *
 *  LOGIC:
 *   ret = copy of r1
 *   for each item x in r2:
 *     if x is not in ret:
 *          override
 *     elif x in ret is NOT of the same type:
 *         override
 *     elif x is an array of tables:
 *         append r2.x to ret.x
 *     elif x is a table:
 *         merge r2.x to ret.x
 *     else:
 *         override
 */
toml_result_t toml_merge(const toml_result_t *r1, const toml_result_t *r2) {
  const char *reason = "";
  toml_result_t ret = {0};
  pool_t *pool = 0;
  if (!r1->ok) {
    reason = "param error: r1 not ok";
    goto bail;
  }
  if (!r2->ok) {
    reason = "param error: r2 not ok";
    goto bail;
  }
  {
    pool_t *r1pool = (pool_t *)r1->__internal;
    pool_t *r2pool = (pool_t *)r2->__internal;
    pool = pool_create(r1pool->top + r2pool->top);
    if (!pool) {
      reason = "out of memory";
      goto bail;
    }
  }

  // Make a copy of r1
  if (datum_copy(&ret.toptab, r1->toptab, pool, &reason)) {
    goto bail;
  }

  // Merge r2 into the result
  if (datum_merge(&ret.toptab, r2->toptab, pool, &reason)) {
    goto bail;
  }

  ret.ok = 1;
  ret.__internal = pool;
  return ret;

bail:
  pool_destroy(pool);
  snprintf(ret.errmsg, sizeof(ret.errmsg), "%s", reason);
  return ret;
}

bool toml_equiv(const toml_result_t *r1, const toml_result_t *r2) {
  if (!(r1->ok && r2->ok)) {
    return false;
  }
  return datum_equiv(r1->toptab, r2->toptab);
}

/**
 * Find a key in a toml_table. Return the value of the key if found,
 * or a TOML_UNKNOWN otherwise.
 */
toml_datum_t toml_get(toml_datum_t datum, const char *key) {
  toml_datum_t ret = {0};
  if (datum.type == TOML_TABLE) {
    int n = datum.u.tab.size;
    const char **pkey = datum.u.tab.key;
    toml_datum_t *pvalue = datum.u.tab.value;
    for (int i = 0; i < n; i++) {
      if (0 == strcmp(pkey[i], key)) {
        return pvalue[i];
      }
    }
  }
  return ret;
}

/**
 * Locate a value starting from a toml_table. Return the value of the key if
 * found, or a TOML_UNKNOWN otherwise.
 *
 * Note: the multipart-key is separated by DOT, and must not have any escape
 * chars.
 */
toml_datum_t toml_seek(toml_datum_t table, const char *multipart_key) {
  if (table.type != TOML_TABLE) {
    return DATUM_ZERO;
  }

  // Make a mutable copy of the multipart_key for splitting
  char buf[256];
  if (copystring(buf, sizeof(buf), multipart_key)) {
    return DATUM_ZERO;
  }

  // Go through the multipart name part by part.
  char *p = buf;            // start of current key
  char *q = strchr(p, '.'); // end of current key
  toml_datum_t datum = table;
  while (q && datum.type == TOML_TABLE) {
    *q = 0;
    datum = toml_get(datum, p);
    if (datum.type == TOML_TABLE) {
      p = q + 1;
      q = strchr(p, '.');
    }
  }

  if (!q && datum.type == TOML_TABLE) {
    return toml_get(datum, p);
  }

  return DATUM_ZERO;
}

/**
 *  Return the default options.
 */
toml_option_t toml_default_option(void) {
  toml_option_t opt = {0, realloc, free};
  return opt;
}

/**
 *  Override the current options.
 */
void toml_set_option(toml_option_t opt) { toml_option = opt; }

/**
 *  Free the result returned by toml_parse().
 */
void toml_free(toml_result_t result) {
  datum_free(&result.toptab);
  pool_destroy((pool_t *)result.__internal);
}

/**
 *  Parse a toml document.
 */
toml_result_t toml_parse_file_ex(const char *fname) {
  toml_result_t result = {0};
  FILE *fp = fopen(fname, "r");
  if (!fp) {
    snprintf(result.errmsg, sizeof(result.errmsg), "fopen: %s", fname);
    return result;
  }
  result = toml_parse_file(fp);
  fclose(fp);
  return result;
}

/**
 *  Parse a toml document.
 */
toml_result_t toml_parse_file(FILE *fp) {
  toml_result_t result = {0};
  char *buf = 0;
  int top, max; // index into buf[]
  top = max = 0;

  // Read file into memory
  while (!feof(fp)) {
    assert(top <= max);
    if (top == max) {
      // need to extend buf[]
      int64_t tmpmax64 = (int64_t)max * 3 / 2 + 1000;
      int tmpmax = (tmpmax64 > INT_MAX - 1) ? INT_MAX - 1 : (int)tmpmax64;
      if (tmpmax == INT_MAX - 1) {
        snprintf(result.errmsg, sizeof(result.errmsg), "file is too big");
        FREE(buf);
        return result;
      }
      // add an extra byte for terminating NUL
      char *tmp = REALLOC(buf, tmpmax + 1);
      if (!tmp) {
        snprintf(result.errmsg, sizeof(result.errmsg), "out of memory");
        FREE(buf);
        return result;
      }
      buf = tmp;
      max = tmpmax;
    }

    errno = 0;
    top += fread(buf + top, 1, max - top, fp);
    if (ferror(fp)) {
      snprintf(result.errmsg, sizeof(result.errmsg), "%s",
               errno ? strerror(errno) : "Error reading file");
      FREE(buf);
      return result;
    }
  }
  buf[top] = 0; // NUL terminator

  result = toml_parse(buf, top);
  FREE(buf);
  return result;
}

/**
 *  Parse a toml document.
 */
toml_result_t toml_parse(const char *src, int len) {
  toml_result_t result = {0};
  parser_t parser = {0};
  parser_t *pp = &parser;

  // Check that src is NUL terminated.
  if (src[len]) {
    snprintf(result.errmsg, sizeof(result.errmsg),
             "src[] must be NUL terminated");
    goto bail;
  }

  // If user insists, check that src[] is a valid utf8 string.
  if (toml_option.check_utf8) {
    int line = 1; // keeps track of line number
    for (int i = 0; i < len;) {
      uint32_t ch;
      int n = utf8_to_ucs(src + i, len - i, &ch);
      if (n < 0) {
        snprintf(result.errmsg, sizeof(result.errmsg),
                 "invalid UTF8 char on line %d", line);
        goto bail;
      }
      if (0xD800 <= ch && ch <= 0xDFFF) {
        // explicitly prohibit surrogates (non-scalar unicode code point)
        snprintf(result.errmsg, sizeof(result.errmsg),
                 "invalid UTF8 char \\u%04x on line %d", ch, line);
        goto bail;
      }
      line += (ch == '\n' ? 1 : 0);
      i += n;
    }
  }

  // Initialize parser
  pp->toptab = mkdatum(TOML_TABLE);
  pp->curtab = &pp->toptab;
  pp->ebuf.ptr = result.errmsg; // parse error will be printed into pp->ebuf
  pp->ebuf.len = sizeof(result.errmsg);

  // Alloc memory pool
  pp->pool =
      pool_create(len + 10); // add some extra bytes for NUL term and safety
  if (!pp->pool) {
    snprintf(result.errmsg, sizeof(result.errmsg), "out of memory");
    goto bail;
  }

  // Initialize scanner. Scan error will be printed into pp->ebuf.
  scan_init(&pp->scanner, src, len, pp->ebuf.ptr, pp->ebuf.len);

  // Keep parsing until FIN
  for (;;) {
    token_t tok;
    if (scan_key(&pp->scanner, &tok)) {
      goto bail;
    }
    // break on FIN
    if (tok.toktyp == TOK_FIN) {
      break;
    }
    switch (tok.toktyp) {
    case TOK_ENDL: // skip blank lines
      continue;
    case TOK_LBRACK:
      if (parse_std_table_expr(pp, tok)) {
        goto bail;
      }
      break;
    case TOK_LLBRACK:
      if (parse_array_table_expr(pp, tok)) {
        goto bail;
      }
      break;
    default:
      // non-blank line: parse an expression
      if (parse_keyvalue_expr(pp, tok)) {
        goto bail;
      }
      break;
    }
    // each expression must be followed by newline
    if (scan_key(&pp->scanner, &tok)) {
      goto bail;
    }
    if (tok.toktyp == TOK_FIN || tok.toktyp == TOK_ENDL) {
      continue;
    }
    SETERROR(pp->ebuf, tok.lineno, "ENDL expected");
    goto bail;
  }

  // return result
  result.ok = true;
  result.toptab = pp->toptab;
  result.__internal = (void *)pp->pool;
  return result;

bail:
  // return error
  datum_free(&pp->toptab);
  pool_destroy(pp->pool);
  result.ok = false;
  if (result.errmsg[0] == '\0') {
    assert(0);
    snprintf(result.errmsg, sizeof(result.errmsg), "Error parsing TOML file");
  }
  return result;
}

// Convert a (LITSTRING, LIT, MLLITSTRING, MLSTRING, or STRING) token to a
// datum.
static int token_to_string(parser_t *pp, token_t tok, toml_datum_t *ret) {
  *ret = mkdatum(TOML_STRING);
  span_t span;
  DO(parse_norm(pp, tok, &span));
  ret->u.str.ptr = (char *)span.ptr;
  ret->u.str.len = span.len;
  return 0;
}

// Convert a TIME/DATE/DATETIME/DATETIMETZ to a datum
static int token_to_timestamp(parser_t *pp, token_t tok, toml_datum_t *ret) {
  (void)pp;
  static const toml_type_t map[] = {[TOK_TIME] = TOML_TIME,
                                    [TOK_DATE] = TOML_DATE,
                                    [TOK_DATETIME] = TOML_DATETIME,
                                    [TOK_DATETIMETZ] = TOML_DATETIMETZ};
  *ret = mkdatum(map[tok.toktyp]);
  ret->u.ts.year = tok.u.tsval.year;
  ret->u.ts.month = tok.u.tsval.month;
  ret->u.ts.day = tok.u.tsval.day;
  ret->u.ts.hour = tok.u.tsval.hour;
  ret->u.ts.minute = tok.u.tsval.minute;
  ret->u.ts.second = tok.u.tsval.sec;
  ret->u.ts.usec = tok.u.tsval.usec;
  ret->u.ts.tz = tok.u.tsval.tz;
  return 0;
}

// Convert an int64 token to a datum.
static int token_to_int64(parser_t *pp, token_t tok, toml_datum_t *ret) {
  (void)pp;
  assert(tok.toktyp == TOK_INTEGER);
  *ret = mkdatum(TOML_INT64);
  ret->u.int64 = tok.u.int64;
  return 0;
}

// Convert a fp64 token to a datum.
static int token_to_fp64(parser_t *pp, token_t tok, toml_datum_t *ret) {
  (void)pp;
  assert(tok.toktyp == TOK_FLOAT);
  *ret = mkdatum(TOML_FP64);
  ret->u.fp64 = tok.u.fp64;
  return 0;
}

// Convert a boolean token to a datum.
static int token_to_boolean(parser_t *pp, token_t tok, toml_datum_t *ret) {
  (void)pp;
  assert(tok.toktyp == TOK_BOOL);
  *ret = mkdatum(TOML_BOOLEAN);
  ret->u.boolean = tok.u.b1;
  return 0;
}

// Parse a multipart key. Return 0 on success, -1 otherwise.
static int parse_key(parser_t *pp, token_t tok, keypart_t *ret_keypart) {
  ret_keypart->nspan = 0;
  // key = simple-key | dotted_key
  // simple-key = STRING | LITSTRING | LIT
  // dotted-key = simple-key (DOT simple-key)+
  if (tok.toktyp != TOK_STRING && tok.toktyp != TOK_LITSTRING &&
      tok.toktyp != TOK_LIT) {
    return SETERROR(pp->ebuf, tok.lineno, "missing key");
  }

  int n = 0;
  span_t *kpspan = ret_keypart->span;

  // Normalize the first keypart
  if (parse_norm(pp, tok, &kpspan[n])) {
    return SETERROR(pp->ebuf, tok.lineno,
                    "unable to normalize string; probably a unicode issue");
  }
  n++;

  // Scan and normalize the second to last keypart
  while (1) {
    scanner_state_t mark = scan_mark(&pp->scanner);

    // Eat the dot if it is there
    DO(scan_key(&pp->scanner, &tok));

    // If not a dot, we are done with keyparts.
    if (tok.toktyp != TOK_DOT) {
      scan_restore(&pp->scanner, mark);
      break;
    }

    // Scan the n-th key
    DO(scan_key(&pp->scanner, &tok));

    if (tok.toktyp != TOK_STRING && tok.toktyp != TOK_LITSTRING &&
        tok.toktyp != TOK_LIT) {
      return SETERROR(pp->ebuf, tok.lineno, "expects a string in dotted-key");
    }

    if (n >= KEYPARTMAX) {
      return SETERROR(pp->ebuf, tok.lineno, "too many key parts");
    }

    // Normalize the n-th key.
    DO(parse_norm(pp, tok, &kpspan[n]));
    n++;
  }

  // This key has n parts.
  ret_keypart->nspan = n;
  return 0;
}

// Starting at toptab, descend following keypart[]. If a key does not
// exist in the current table, create a new table entry for the
// key. Returns the final table represented by the key.
static toml_datum_t *descend_keypart(parser_t *pp, int lineno,
                                     toml_datum_t *toptab, keypart_t *keypart,
                                     bool stdtabexpr) {
  toml_datum_t *tab = toptab; // current tab

  for (int i = 0; i < keypart->nspan; i++) {
    const char *reason;
    // Find the i-th keypart
    int j = tab_find(tab, keypart->span[i]);
    // Not found: add a new (key, tab) pair.
    if (j < 0) {
      toml_datum_t newtab = mkdatum(TOML_TABLE);
      newtab.flag |= stdtabexpr ? FLAG_STDEXPR : 0;
      if (tab_add(tab, keypart->span[i], newtab, &reason)) {
        SETERROR(pp->ebuf, lineno, "%s", reason);
        return NULL;
      }
      tab = &tab->u.tab.value[tab->u.tab.size - 1]; // descend
      continue;
    }

    // Found: extract the value of the key.
    toml_datum_t *value = &tab->u.tab.value[j];

    // If the value is a table, descend.
    if (value->type == TOML_TABLE) {
      tab = value; // descend
      continue;
    }

    // If the value is an array: locate the last entry and descend.
    if (value->type == TOML_ARRAY) {
      // If empty: error.
      if (value->u.arr.size <= 0) {
        SETERROR(pp->ebuf, lineno, "array %s has no elements",
                 keypart->span[i].ptr);
        return NULL;
      }

      // Extract the last element of the array.
      value = &value->u.arr.elem[value->u.arr.size - 1];

      // It must be a table!
      if (value->type != TOML_TABLE) {
        SETERROR(pp->ebuf, lineno, "array %s must be array of tables",
                 keypart->span[i].ptr);
        return NULL;
      }
      tab = value; // descend
      continue;
    }

    // key not found
    SETERROR(pp->ebuf, lineno, "cannot locate table at key %s",
             keypart->span[i].ptr);
    return NULL;
  }

  // Return the table corresponding to the keypart[].
  return tab;
}

// Recursively set flags on datum
static void set_flag_recursive(toml_datum_t *datum, uint32_t flag) {
  datum->flag |= flag;
  switch (datum->type) {
  case TOML_ARRAY:
    for (int i = 0, top = datum->u.arr.size; i < top; i++) {
      set_flag_recursive(&datum->u.arr.elem[i], flag);
    }
    break;
  case TOML_TABLE:
    for (int i = 0, top = datum->u.tab.size; i < top; i++) {
      set_flag_recursive(&datum->u.tab.value[i], flag);
    }
    break;
  default:
    break;
  }
}

// Parse an inline array.
static int parse_inline_array(parser_t *pp, token_t tok,
                              toml_datum_t *ret_datum) {
  assert(tok.toktyp == TOK_LBRACK);
  *ret_datum = mkdatum(TOML_ARRAY);
  int need_comma = 0;

  // loop until RBRACK
  for (;;) {
    // skip ENDL
    do {
      DO(scan_value(&pp->scanner, &tok));
    } while (tok.toktyp == TOK_ENDL);

    // If got an RBRACK: done!
    if (tok.toktyp == TOK_RBRACK) {
      break;
    }

    // If got a COMMA: check if it is expected.
    if (tok.toktyp == TOK_COMMA) {
      if (need_comma) {
        need_comma = 0;
        continue;
      }
      return SETERROR(pp->ebuf, tok.lineno,
                      "syntax error while parsing array: unexpected comma");
    }

    // Not a comma, but need a comma: error!
    if (need_comma) {
      return SETERROR(pp->ebuf, tok.lineno,
                      "syntax error while parsing array: missing comma");
    }

    // This is a valid value!

    // Add the value to the array.
    const char *reason;
    toml_datum_t *pelem = arr_emplace(ret_datum, &reason);
    if (!pelem) {
      return SETERROR(pp->ebuf, tok.lineno, "while parsing array: %s", reason);
    }

    // Parse the value and save into array.
    DO(parse_val(pp, tok, pelem));

    // Need comma before the next value.
    need_comma = 1;
  }

  // Set the INLINE flag for all things in this array.
  set_flag_recursive(ret_datum, FLAG_INLINED);
  return 0;
}

// Parse an inline table.
static int parse_inline_table(parser_t *pp, token_t tok,
                              toml_datum_t *ret_datum) {
  assert(tok.toktyp == TOK_LBRACE);
  *ret_datum = mkdatum(TOML_TABLE);
  bool need_comma = 0;
  bool was_comma = 0;

  // loop until RBRACE
  for (;;) {
    DO(scan_key(&pp->scanner, &tok));

    // Got an RBRACE: done!
    if (tok.toktyp == TOK_RBRACE) {
      if (was_comma) {
        /*
        return SETERROR(pp->ebuf, tok.lineno,
                        "extra comma before closing brace");
        */
        // extra comma before RBRACE is allowed for v1.1
        (void)0;
      }
      break;
    }

    // Got a comma: check if it is expected.
    if (tok.toktyp == TOK_COMMA) {
      if (need_comma) {
        need_comma = 0, was_comma = 1;
        continue;
      }
      return SETERROR(pp->ebuf, tok.lineno, "unexpected comma");
    }

    // Newline not allowed in inline table.
    // newline is allowed in v1.1
    if (tok.toktyp == TOK_ENDL) {
      // return SETERROR(pp->ebuf, tok.lineno, "unexpected newline");
      continue;
    }

    // Not a comma, but need a comma: error!
    if (need_comma) {
      return SETERROR(pp->ebuf, tok.lineno, "missing comma");
    }

    // Get the keyparts
    keypart_t keypart = {0};
    int keylineno = tok.lineno;
    DO(parse_key(pp, tok, &keypart));

    // Descend to one keypart before last
    span_t lastkeypart = keypart.span[--keypart.nspan];
    toml_datum_t *tab =
        descend_keypart(pp, keylineno, ret_datum, &keypart, false);
    if (!tab) {
      return -1;
    }

    // If tab is a previously declared inline table: error.
    if (tab->flag & FLAG_INLINED) {
      return SETERROR(pp->ebuf, tok.lineno, "inline table cannot be extended");
    }

    // We are explicitly defining it now.
    tab->flag |= FLAG_EXPLICIT;

    // match EQUAL
    DO(scan_value(&pp->scanner, &tok));

    if (tok.toktyp != TOK_EQUAL) {
      if (tok.toktyp == TOK_ENDL) {
        return SETERROR(pp->ebuf, tok.lineno, "unexpected newline");
      } else {
        return SETERROR(pp->ebuf, tok.lineno, "missing '='");
      }
    }

    // obtain the value
    toml_datum_t value;
    DO(scan_value(&pp->scanner, &tok));
    DO(parse_val(pp, tok, &value));

    // Add the value to tab.
    const char *reason;
    if (tab_add(tab, lastkeypart, value, &reason)) {
      return SETERROR(pp->ebuf, tok.lineno, "%s", reason);
    }
    need_comma = 1, was_comma = 0;
  }

  set_flag_recursive(ret_datum, FLAG_INLINED);
  return 0;
}

// Parse a value.
static int parse_val(parser_t *pp, token_t tok, toml_datum_t *ret) {
  // val = string / boolean / array / inline-table / date-time / float / integer
  switch (tok.toktyp) {
  case TOK_STRING:
  case TOK_MLSTRING:
  case TOK_LITSTRING:
  case TOK_MLLITSTRING:
    return token_to_string(pp, tok, ret);
  case TOK_TIME:
  case TOK_DATE:
  case TOK_DATETIME:
  case TOK_DATETIMETZ:
    return token_to_timestamp(pp, tok, ret);
  case TOK_INTEGER:
    return token_to_int64(pp, tok, ret);
  case TOK_FLOAT:
    return token_to_fp64(pp, tok, ret);
  case TOK_BOOL:
    return token_to_boolean(pp, tok, ret);
  case TOK_LBRACK: // inline-array
    return parse_inline_array(pp, tok, ret);
  case TOK_LBRACE: // inline-table
    return parse_inline_table(pp, tok, ret);
  default:
    break;
  }
  return SETERROR(pp->ebuf, tok.lineno, "missing value");
}

// Parse a standard table expression, and set the curtab of the parser
// to the table referenced.  A standard table expression is a line
// like [a.b.c.d].
static int parse_std_table_expr(parser_t *pp, token_t tok) {
  // std-table = [ key ]
  // Eat the [
  assert(tok.toktyp == TOK_LBRACK); // [ ate by caller

  // Read the first keypart
  DO(scan_key(&pp->scanner, &tok));

  // Extract the keypart[]
  int keylineno = tok.lineno;
  keypart_t keypart;
  DO(parse_key(pp, tok, &keypart));

  // Eat the ]
  DO(scan_key(&pp->scanner, &tok));
  if (tok.toktyp != TOK_RBRACK) {
    return SETERROR(pp->ebuf, tok.lineno, "missing right-bracket");
  }

  // Descend to one keypart before last.
  span_t lastkeypart = keypart.span[--keypart.nspan];

  // Descend keypart from the toptab.
  toml_datum_t *tab =
      descend_keypart(pp, keylineno, &pp->toptab, &keypart, true);
  if (!tab) {
    return -1;
  }

  // Look for the last keypart in the final tab
  int j = tab_find(tab, lastkeypart);
  if (j < 0) {
    // If not found: add it.
    if (tab->flag & FLAG_INLINED) {
      return SETERROR(pp->ebuf, keylineno, "inline table cannot be extended");
    }
    const char *reason;
    toml_datum_t newtab = mkdatum(TOML_TABLE);
    newtab.flag |= FLAG_STDEXPR;
    if (tab_add(tab, lastkeypart, newtab, &reason)) {
      return SETERROR(pp->ebuf, keylineno, "%s", reason);
    }
    // this is the new tab
    tab = &tab->u.tab.value[tab->u.tab.size - 1];
  } else {
    // Found: check for errors
    tab = &tab->u.tab.value[j];
    if (tab->flag & FLAG_EXPLICIT) {
      /*
        This is not OK:
        [x.y.z]
        [x.y.z]

        but this is OK:
        [x.y.z]
        [x]
      */
      return SETERROR(pp->ebuf, keylineno, "table defined more than once");
    }
    if (!(tab->flag & FLAG_STDEXPR)) {
      /*
      [t1]			# OK
      t2.t3.v = 0		# OK
      [t1.t2]   		# should FAIL  - t2 was non-explicit but was not
      created by std-table-expr
      */
      return SETERROR(pp->ebuf, keylineno, "table defined before");
    }
  }

  // Set explicit flag on tab
  tab->flag |= FLAG_EXPLICIT;

  // Set tab as curtab of the parser
  pp->curtab = tab;
  return 0;
}

// Parse an array table expression, and set the curtab of the parser
// to the table referenced. A standard array table expresison is a line
// like [[a.b.c.d]].
static int parse_array_table_expr(parser_t *pp, token_t tok) {
  // array-table = [[ key ]]
  assert(tok.toktyp == TOK_LLBRACK); // [[ ate by caller

  // Read the first keypart
  DO(scan_key(&pp->scanner, &tok));

  int keylineno = tok.lineno;
  keypart_t keypart;
  DO(parse_key(pp, tok, &keypart));

  // eat the ]]
  token_t rrb;
  DO(scan_key(&pp->scanner, &rrb));
  if (rrb.toktyp != TOK_RRBRACK) {
    return SETERROR(pp->ebuf, rrb.lineno, "missing ']]'");
  }

  // remove the last keypart from keypart[]
  span_t lastkeypart = keypart.span[--keypart.nspan];

  // descend the key from the toptab
  toml_datum_t *tab = &pp->toptab;
  for (int i = 0; i < keypart.nspan; i++) {
    span_t curkey = keypart.span[i];
    int j = tab_find(tab, curkey);
    if (j < 0) {
      // If not found: add a new (key,tab) pair
      const char *reason;
      toml_datum_t newtab = mkdatum(TOML_TABLE);
      newtab.flag |= FLAG_STDEXPR;
      if (tab_add(tab, curkey, newtab, &reason)) {
        return SETERROR(pp->ebuf, keylineno, "%s", reason);
      }
      tab = &tab->u.tab.value[tab->u.tab.size - 1];
      continue;
    }

    // Found: get the value
    toml_datum_t *value = &tab->u.tab.value[j];

    // If value is table, then point to that table and continue descent.
    if (value->type == TOML_TABLE) {
      tab = value;
      continue;
    }

    // If value is an array of table, point to the last element of the array and
    // continue descent.
    if (value->type == TOML_ARRAY) {
      if (value->flag & FLAG_INLINED) {
        return SETERROR(pp->ebuf, keylineno, "cannot expand array %s",
                        curkey.ptr);
      }
      if (value->u.arr.size <= 0) {
        return SETERROR(pp->ebuf, keylineno, "array %s has no elements",
                        curkey.ptr);
      }
      value = &value->u.arr.elem[value->u.arr.size - 1];
      if (value->type != TOML_TABLE) {
        return SETERROR(pp->ebuf, keylineno, "array %s must be array of tables",
                        curkey.ptr);
      }
      tab = value;
      continue;
    }

    // keypart not found
    return SETERROR(pp->ebuf, keylineno, "cannot locate table at key %s",
                    curkey.ptr);
  }

  // For the final keypart, make sure entry at key is an array of tables
  const char *reason;
  int idx = tab_find(tab, lastkeypart);
  if (idx == -1) {
    // If not found, add an array of table.
    if (tab_add(tab, lastkeypart, mkdatum(TOML_ARRAY), &reason)) {
      return SETERROR(pp->ebuf, keylineno, "%s", reason);
    }
    idx = tab_find(tab, lastkeypart);
    assert(idx >= 0);
  }
  // Check that this is an array.
  if (tab->u.tab.value[idx].type != TOML_ARRAY) {
    return SETERROR(pp->ebuf, keylineno, "entry must be an array");
  }
  // Add an empty table to the array
  toml_datum_t *arr = &tab->u.tab.value[idx];
  if (arr->flag & FLAG_INLINED) {
    return SETERROR(pp->ebuf, keylineno, "cannot extend a static array");
  }
  toml_datum_t *pelem = arr_emplace(arr, &reason);
  if (!pelem) {
    return SETERROR(pp->ebuf, keylineno, "%s", reason);
  }
  *pelem = mkdatum(TOML_TABLE);

  // Set the last element of this array as curtab of the parser
  pp->curtab = &arr->u.arr.elem[arr->u.arr.size - 1];
  assert(pp->curtab->type == TOML_TABLE);

  return 0;
}

// Parse an expression. A toml doc is just a list of expressions.
static int parse_keyvalue_expr(parser_t *pp, token_t tok) {
  // Obtain the key
  int keylineno = tok.lineno;
  keypart_t keypart;
  DO(parse_key(pp, tok, &keypart));

  // match the '='
  DO(scan_key(&pp->scanner, &tok));
  if (tok.toktyp != TOK_EQUAL) {
    return SETERROR(pp->ebuf, tok.lineno, "expect '='");
  }

  // Obtain the value
  toml_datum_t val;
  DO(scan_value(&pp->scanner, &tok));
  DO(parse_val(pp, tok, &val));

  // Locate the last table using keypart[]
  const char *reason;
  toml_datum_t *tab = pp->curtab;
  for (int i = 0; i < keypart.nspan - 1; i++) {
    int j = tab_find(tab, keypart.span[i]);
    if (j < 0) {
      if (i > 0 && (tab->flag & FLAG_EXPLICIT)) {
        return SETERROR(
            pp->ebuf, keylineno,
            "cannot extend a previously defined table using dotted expression");
      }
      toml_datum_t newtab = mkdatum(TOML_TABLE);
      if (tab_add(tab, keypart.span[i], newtab, &reason)) {
        return SETERROR(pp->ebuf, keylineno, "%s", reason);
      }
      tab = &tab->u.tab.value[tab->u.tab.size - 1];
      continue;
    }
    toml_datum_t *value = &tab->u.tab.value[j];
    if (value->type == TOML_TABLE) {
      tab = value;
      continue;
    }
    if (value->type == TOML_ARRAY) {
      return SETERROR(pp->ebuf, keylineno,
                      "encountered previously declared array '%s'",
                      keypart.span[i].ptr);
    }
    return SETERROR(pp->ebuf, keylineno, "cannot locate table at '%s'",
                    keypart.span[i].ptr);
  }

  // Check for disallowed situations.
  if (tab->flag & FLAG_INLINED) {
    return SETERROR(pp->ebuf, keylineno, "inline table cannot be extended");
  }
  if (keypart.nspan > 1 && (tab->flag & FLAG_EXPLICIT)) {
    return SETERROR(
        pp->ebuf, keylineno,
        "cannot extend a previously defined table using dotted expression");
  }

  // Add a new key/value for tab.
  if (tab_add(tab, keypart.span[keypart.nspan - 1], val, &reason)) {
    return SETERROR(pp->ebuf, keylineno, "%s", reason);
  }

  return 0;
}

// Normalize a LIT/STRING/MLSTRING/LITSTRING/MLLITSTRING
// -> unescape all escaped chars
// The returned string is allocated out of pp->sbuf[]
static int parse_norm(parser_t *pp, token_t tok, span_t *ret_span) {
  // Allocate a buffer to store the normalized string. Add one
  // extra-byte for terminating NUL.
  char *p = pool_alloc(pp->pool, tok.str.len + 1);
  if (!p) {
    return SETERROR(pp->ebuf, tok.lineno, "out of memory");
  }

  // Copy from token string into buffer
  memcpy(p, tok.str.ptr, tok.str.len);
  p[tok.str.len] = 0; // additional NUL term for safety

  ret_span->ptr = p;
  ret_span->len = tok.str.len;

  switch (tok.toktyp) {
  case TOK_LIT:
  case TOK_LITSTRING:
  case TOK_MLLITSTRING:
    // no need to handle escape chars
    return 0;

  case TOK_STRING:
  case TOK_MLSTRING:
    // need to handle escape chars
    break;

  default:
    return SETERROR(pp->ebuf, 0, "internal: arg must be a string");
  }

  // if there is no escape char, then done!
  if (!tok.u.escp) {
    return 0; // success
  }

  // p points to the backslash
  p += (tok.u.escp - tok.str.ptr);
  assert(p - ret_span->ptr == tok.u.escp - tok.str.ptr);
  assert(*p == '\\');

  // Normalize the escaped chars
  char *dst = p;
  while (*p) {
    if (*p != '\\') {
      *dst++ = *p++;
      continue;
    }
    switch (p[1]) {
    case '"':
    case '\\':
      *dst++ = p[1];
      p += 2;
      continue;
    case 'b':
      *dst++ = '\b';
      p += 2;
      continue;
    case 't':
      *dst++ = '\t';
      p += 2;
      continue;
    case 'n':
      *dst++ = '\n';
      p += 2;
      continue;
    case 'f':
      *dst++ = '\f';
      p += 2;
      continue;
    case 'r':
      *dst++ = '\r';
      p += 2;
      continue;
    case 'e':
      *dst++ = '\e';
      p += 2;
      continue;
    case 'x': {
      char buf[3];
      memcpy(buf, p + 2, 2);
      buf[2] = 0;
      int32_t ucs = strtol(buf, 0, 16);
      int n = ucs_to_utf8(ucs, dst);
      if (n < 0) {
        return SETERROR(pp->ebuf, tok.lineno, "error converting UCS %s to UTF8",
                        buf);
      }
      dst += n;
      p += 2 + 2; // \xNN
      continue;
    }
    case 'u':
    case 'U': {
      char buf[9];
      int sz = (p[1] == 'u' ? 4 : 8);
      memcpy(buf, p + 2, sz);
      buf[sz] = 0;
      int32_t ucs = strtol(buf, 0, 16);
      if (0xD800 <= ucs && ucs <= 0xDFFF) {
        // explicitly prohibit surrogates (non-scalar unicode code point)
        return SETERROR(pp->ebuf, tok.lineno, "invalid UTF8 char \\u%04x", ucs);
      }
      int n = ucs_to_utf8(ucs, dst);
      if (n < 0) {
        return SETERROR(pp->ebuf, tok.lineno, "error converting UCS %s to UTF8",
                        buf);
      }
      dst += n;
      p += 2 + sz; // \uNNNN or \UNNNNNNNN
      continue;
    }

    case ' ':
    case '\t':
    case '\r':
      // line-ending backslash
      // --- allow for extra whitespace chars after backslash
      // --- skip until newline
      p++;                     // skip the escape char
      p += strspn(p, " \t\r"); // skip whitespaces
      if (*p != '\n') {
        return SETERROR(pp->ebuf, tok.lineno,
                        "unexpected char after line-ending backslash");
      }
      // fallthru
    case '\n':
      // skip all whitespaces including newline
      p++;
      p += strspn(p, " \t\r\n");
      continue;
    default:
      *dst++ = *p++;
      continue;
    }
  }
  *dst = 0;
  ret_span->len = dst - ret_span->ptr;
  return 0;
}
