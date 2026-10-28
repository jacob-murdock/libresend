#include "internal.h"

#include <float.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* RFC 8259 whitespace only. Locale isspace would also accept form feed and vertical tab. */
static int json_is_ws(unsigned char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

static int json_is_alnum(unsigned char c) {
    return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
}

int resend_json_object_begin(resend_buf *b) { return resend_buf_putc(b, '{'); }
int resend_json_object_end(resend_buf *b) { return resend_buf_putc(b, '}'); }
int resend_json_array_begin(resend_buf *b) { return resend_buf_putc(b, '['); }
int resend_json_array_end(resend_buf *b) { return resend_buf_putc(b, ']'); }
int resend_json_comma(resend_buf *b) { return resend_buf_putc(b, ','); }

static int resend_json_escape_put(resend_buf *b, unsigned char c) {
    switch (c) {
    case '"':
        return resend_buf_puts(b, "\\\"");
    case '\\':
        return resend_buf_puts(b, "\\\\");
    case '\b':
        return resend_buf_puts(b, "\\b");
    case '\f':
        return resend_buf_puts(b, "\\f");
    case '\n':
        return resend_buf_puts(b, "\\n");
    case '\r':
        return resend_buf_puts(b, "\\r");
    case '\t':
        return resend_buf_puts(b, "\\t");
    default:
        if (c < 0x20) {
            char hex[7];
            snprintf(hex, sizeof hex, "\\u%04x", c);
            return resend_buf_puts(b, hex);
        }
        return resend_buf_putc(b, (char)c);
    }
}

int resend_json_string(resend_buf *b, const char *s) {
    if (!s)
        s = "";
    if (resend_buf_putc(b, '"') != 0)
        return -1;
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        if (resend_json_escape_put(b, *p) != 0)
            return -1;
    }
    return resend_buf_putc(b, '"');
}

int resend_json_key(resend_buf *b, const char *key) {
    if (resend_json_string(b, key) != 0)
        return -1;
    return resend_buf_putc(b, ':');
}

int resend_json_string_array(resend_buf *b, const char *const *items, size_t count) {
    if (resend_json_array_begin(b) != 0)
        return -1;
    for (size_t i = 0; i < count; i++) {
        if (i && resend_json_comma(b) != 0)
            return -1;
        if (resend_json_string(b, items[i]) != 0)
            return -1;
    }
    return resend_json_array_end(b);
}

int resend_json_pair_object(resend_buf *b, const resend_pair *pairs, size_t count) {
    if (resend_json_object_begin(b) != 0)
        return -1;
    for (size_t i = 0; i < count; i++) {
        if (i && resend_json_comma(b) != 0)
            return -1;
        if (resend_json_key(b, pairs[i].name) != 0 || resend_json_string(b, pairs[i].value) != 0)
            return -1;
    }
    return resend_json_object_end(b);
}

int resend_json_tag_array(resend_buf *b, const resend_pair *tags, size_t count) {
    if (resend_json_array_begin(b) != 0)
        return -1;
    for (size_t i = 0; i < count; i++) {
        if (i && resend_json_comma(b) != 0)
            return -1;
        if (resend_json_object_begin(b) != 0)
            return -1;
        if (resend_json_key(b, "name") != 0 || resend_json_string(b, tags[i].name) != 0)
            return -1;
        if (resend_json_comma(b) != 0)
            return -1;
        if (resend_json_key(b, "value") != 0 || resend_json_string(b, tags[i].value) != 0)
            return -1;
        if (resend_json_object_end(b) != 0)
            return -1;
    }
    return resend_json_array_end(b);
}

/* Least-significant digit first. */
static int emit_lsd(resend_buf *b, const unsigned char *digs, int n) {
    while (n > 0) {
        if (resend_buf_putc(b, (char)('0' + digs[--n])) != 0)
            return -1;
    }
    return 0;
}

static int emit_u64(resend_buf *b, uint64_t v) {
    unsigned char digs[20];
    int n = 0;
    if (v == 0)
        return resend_buf_putc(b, '0');
    while (v > 0) {
        digs[n++] = (unsigned char)(v % 10);
        v /= 10;
    }
    return emit_lsd(b, digs, n);
}

/* mant * 2^shift, shift >= 0. Decimal digits, no locale. */
static int emit_shifted(resend_buf *b, uint64_t mant, int shift) {
    unsigned char digs[400];
    int n = 0;
    while (mant > 0) {
        digs[n++] = (unsigned char)(mant % 10);
        mant /= 10;
    }
    for (int s = 0; s < shift; s++) {
        int carry = 0;
        for (int i = 0; i < n; i++) {
            int v = digs[i] * 2 + carry;
            digs[i] = (unsigned char)(v % 10);
            carry = v / 10;
        }
        if (carry) {
            if (n >= (int)sizeof digs)
                return -1;
            digs[n++] = (unsigned char)carry;
        }
    }
    return emit_lsd(b, digs, n);
}

/* 1 if n is an integer and was written, 0 if it has a fraction, -1 on error.
 * n is non-negative. Every finite double at or above 2^53 is an integer. */
static int emit_integer(resend_buf *b, double n) {
    uint64_t bits;
    memcpy(&bits, &n, sizeof bits);
    int exp2 = (int)((bits >> 52) & 0x7ff);
    uint64_t frac = bits & 0xfffffffffffffULL;
    if (exp2 == 0)
        return 0;
    uint64_t mant = frac | (1ULL << 52);
    int shift = exp2 - 1023 - 52;
    if (shift < 0) {
        int drop = -shift;
        if (drop >= 53)
            return 0;
        if (mant & ((1ULL << drop) - 1))
            return 0;
        return emit_u64(b, mant >> drop) == 0 ? 1 : -1;
    }
    return emit_shifted(b, mant, shift) == 0 ? 1 : -1;
}

static int emit_mant(resend_buf *b, uint64_t mant, int exp10) {
    char rev[24];
    int width = 0;
    while (mant > 0) {
        rev[width++] = (char)('0' + (mant % 10));
        mant /= 10;
    }
    char digs[24];
    for (int i = 0; i < width; i++)
        digs[i] = rev[width - 1 - i];

    int before = width + exp10;
    if (before >= width) {
        if (resend_buf_put(b, digs, (size_t)width) != 0)
            return -1;
        for (int i = 0; i < before - width; i++) {
            if (resend_buf_putc(b, '0') != 0)
                return -1;
        }
        return 0;
    }
    if (before <= 0) {
        if (resend_buf_puts(b, "0.") != 0)
            return -1;
        for (int i = 0; i < -before; i++) {
            if (resend_buf_putc(b, '0') != 0)
                return -1;
        }
        int end = width;
        while (end > 1 && digs[end - 1] == '0')
            end--;
        return resend_buf_put(b, digs, (size_t)end);
    }
    if (resend_buf_put(b, digs, (size_t)before) != 0)
        return -1;
    int end = width;
    while (end > before && digs[end - 1] == '0')
        end--;
    if (end == before)
        return 0;
    if (resend_buf_putc(b, '.') != 0)
        return -1;
    return resend_buf_put(b, digs + before, (size_t)(end - before));
}

/* Fifteen significant digits, trimmed. The scale stays at or below 2^53 so
 * rounding to an integer is exact. snprintf %g is not used: its decimal mark
 * follows LC_NUMERIC. */
static int emit_fraction(resend_buf *b, double n) {
    int exp10 = 0;
    while (n >= 1e15) {
        n /= 10.0;
        if (++exp10 > 400)
            return -1;
    }
    while (n < 1e14) {
        n *= 10.0;
        if (--exp10 < -400)
            return -1;
    }
    uint64_t mant = (uint64_t)(n + 0.5);
    if (mant >= 1000000000000000ULL) {
        mant /= 10;
        exp10++;
    }
    if (mant == 0)
        return resend_buf_putc(b, '0');
    return emit_mant(b, mant, exp10);
}

int resend_json_number(resend_buf *b, double n) {
    if (n != n || n > DBL_MAX || n < -DBL_MAX)
        return -1;
    if (n < 0) {
        if (resend_buf_putc(b, '-') != 0)
            return -1;
        n = -n;
    }
    if (n == 0.0)
        return resend_buf_putc(b, '0');
    int kind = emit_integer(b, n);
    if (kind != 0)
        return kind < 0 ? -1 : 0;
    return emit_fraction(b, n);
}

const char *resend_json_skip_ws(const char *p, const char *end) {
    while (p < end && json_is_ws((unsigned char)*p))
        p++;
    return p;
}

enum { JSON_MAX_DEPTH = 64 };

static int put_utf8(resend_buf *b, unsigned int cp) {
    unsigned char tmp[4];
    size_t n;
    /* C strings cannot hold U+0000. U+FFFD is the usual stand-in. */
    if (cp == 0)
        return resend_buf_puts(b, "\xEF\xBF\xBD");
    if (cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF))
        return -1;
    if (cp < 0x80) {
        tmp[0] = (unsigned char)cp;
        n = 1;
    } else if (cp < 0x800) {
        tmp[0] = (unsigned char)(0xC0 | (cp >> 6));
        tmp[1] = (unsigned char)(0x80 | (cp & 0x3F));
        n = 2;
    } else if (cp < 0x10000) {
        tmp[0] = (unsigned char)(0xE0 | (cp >> 12));
        tmp[1] = (unsigned char)(0x80 | ((cp >> 6) & 0x3F));
        tmp[2] = (unsigned char)(0x80 | (cp & 0x3F));
        n = 3;
    } else {
        tmp[0] = (unsigned char)(0xF0 | (cp >> 18));
        tmp[1] = (unsigned char)(0x80 | ((cp >> 12) & 0x3F));
        tmp[2] = (unsigned char)(0x80 | ((cp >> 6) & 0x3F));
        tmp[3] = (unsigned char)(0x80 | (cp & 0x3F));
        n = 4;
    }
    return resend_buf_put(b, (const char *)tmp, n);
}

static int parse_hex4(const char **p, const char *end, unsigned int *out) {
    if ((size_t)(end - *p) < 4)
        return -1;
    unsigned int cp = 0;
    for (int i = 0; i < 4; i++) {
        unsigned char h = (unsigned char)(*p)[i];
        cp <<= 4;
        if (h >= '0' && h <= '9')
            cp |= (unsigned int)(h - '0');
        else if (h >= 'a' && h <= 'f')
            cp |= (unsigned int)(h - 'a' + 10);
        else if (h >= 'A' && h <= 'F')
            cp |= (unsigned int)(h - 'A' + 10);
        else
            return -1;
    }
    *p += 4;
    *out = cp;
    return 0;
}

/* One \uXXXX, plus a low surrogate when this is a high surrogate. b NULL skips. */
static int take_unicode_escape(const char **p, const char *end, resend_buf *b) {
    unsigned int cp = 0;
    if (parse_hex4(p, end, &cp) != 0)
        return -1;
    if (cp >= 0xD800 && cp <= 0xDBFF) {
        if ((size_t)(end - *p) < 6 || (*p)[0] != '\\' || (*p)[1] != 'u')
            return -1;
        *p += 2;
        unsigned int low = 0;
        if (parse_hex4(p, end, &low) != 0 || low < 0xDC00 || low > 0xDFFF)
            return -1;
        cp = 0x10000u + (((cp - 0xD800u) << 10) | (low - 0xDC00u));
    } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
        return -1;
    }
    if (!b)
        return 0;
    return put_utf8(b, cp);
}

int resend_json_parse_string(const char *p, const char *end, char **out, const char **next) {
    if (p >= end || *p != '"')
        return -1;
    p++;
    resend_buf b;
    resend_buf_init(&b);
    while (p < end) {
        unsigned char c = (unsigned char)*p++;
        if (c == '"') {
            if (!b.data) {
                b.data = strdup("");
                if (!b.data)
                    return -1;
                b.len = 0;
                b.cap = 1;
            }
            *out = b.data;
            *next = p;
            return 0;
        }
        if (c == '\\') {
            if (p >= end) {
                resend_buf_free(&b);
                return -1;
            }
            unsigned char e = (unsigned char)*p++;
            switch (e) {
            case '"':
            case '\\':
            case '/':
                c = e;
                break;
            case 'b':
                c = '\b';
                break;
            case 'f':
                c = '\f';
                break;
            case 'n':
                c = '\n';
                break;
            case 'r':
                c = '\r';
                break;
            case 't':
                c = '\t';
                break;
            case 'u':
                if (take_unicode_escape(&p, end, &b) != 0) {
                    resend_buf_free(&b);
                    return -1;
                }
                continue;
            default:
                resend_buf_free(&b);
                return -1;
            }
        } else if (c < 0x20) {
            resend_buf_free(&b);
            return -1;
        }
        if (resend_buf_putc(&b, (char)c) != 0) {
            resend_buf_free(&b);
            return -1;
        }
    }
    resend_buf_free(&b);
    return -1;
}

static int skip_string(const char *p, const char *end, const char **next) {
    if (p >= end || *p != '"')
        return -1;
    p++;
    while (p < end) {
        unsigned char c = (unsigned char)*p++;
        if (c == '"') {
            *next = p;
            return 0;
        }
        if (c == '\\') {
            if (p >= end)
                return -1;
            unsigned char e = (unsigned char)*p++;
            switch (e) {
            case '"':
            case '\\':
            case '/':
            case 'b':
            case 'f':
            case 'n':
            case 'r':
            case 't':
                break;
            case 'u':
                if (take_unicode_escape(&p, end, NULL) != 0)
                    return -1;
                break;
            default:
                return -1;
            }
        } else if (c < 0x20) {
            return -1;
        }
    }
    return -1;
}

static int skip_lit(const char *p, const char *end, const char *lit, const char **next) {
    size_t n = strlen(lit);
    if ((size_t)(end - p) < n || memcmp(p, lit, n) != 0)
        return -1;
    if (p + n < end) {
        unsigned char c = (unsigned char)p[n];
        if (c == '_' || json_is_alnum(c))
            return -1;
    }
    *next = p + n;
    return 0;
}

static int skip_number(const char *p, const char *end, const char **next) {
    if (p < end && *p == '-')
        p++;
    if (p >= end || *p < '0' || *p > '9')
        return -1;
    if (*p == '0') {
        p++;
    } else {
        while (p < end && *p >= '0' && *p <= '9')
            p++;
    }
    if (p < end && *p == '.') {
        p++;
        if (p >= end || *p < '0' || *p > '9')
            return -1;
        while (p < end && *p >= '0' && *p <= '9')
            p++;
    }
    if (p < end && (*p == 'e' || *p == 'E')) {
        p++;
        if (p < end && (*p == '+' || *p == '-'))
            p++;
        if (p >= end || *p < '0' || *p > '9')
            return -1;
        while (p < end && *p >= '0' && *p <= '9')
            p++;
    }
    *next = p;
    return 0;
}

static int skip_value(const char *p, const char *end, const char **next, int depth);
static int walk_object(const char *p, const char *end, const char *key, const char **val,
                       const char **val_end, const char **next, int depth);

static int skip_array(const char *p, const char *end, const char **next, int depth) {
    if (depth >= JSON_MAX_DEPTH || p >= end || *p != '[')
        return -1;
    p++;
    p = resend_json_skip_ws(p, end);
    if (p < end && *p == ']') {
        *next = p + 1;
        return 0;
    }
    for (;;) {
        const char *start = p;
        if (skip_value(p, end, &p, depth + 1) != 0 || p == start)
            return -1;
        p = resend_json_skip_ws(p, end);
        if (p < end && *p == ',') {
            p = resend_json_skip_ws(p + 1, end);
            if (p >= end || *p == ']')
                return -1;
            continue;
        }
        if (p < end && *p == ']') {
            *next = p + 1;
            return 0;
        }
        return -1;
    }
}

/* 0 valid (and key found, when key is set), 1 key absent, -1 malformed. */
static int walk_object(const char *p, const char *end, const char *key, const char **val,
                       const char **val_end, const char **next, int depth) {
    if (depth >= JSON_MAX_DEPTH || p >= end || *p != '{')
        return -1;
    p++;
    int found = 0;
    const char *found_val = NULL;
    const char *found_end = NULL;
    p = resend_json_skip_ws(p, end);
    if (p < end && *p == '}') {
        *next = p + 1;
        return key ? 1 : 0;
    }
    for (;;) {
        char *k = NULL;
        if (resend_json_parse_string(p, end, &k, &p) != 0)
            return -1;
        p = resend_json_skip_ws(p, end);
        if (p >= end || *p != ':') {
            free(k);
            return -1;
        }
        p++;
        p = resend_json_skip_ws(p, end);
        int want = key && strcmp(k, key) == 0;
        free(k);
        const char *start = p;
        if (skip_value(p, end, &p, depth + 1) != 0 || p == start)
            return -1;
        if (want) {
            found = 1;
            found_val = start;
            found_end = p;
        }
        p = resend_json_skip_ws(p, end);
        if (p < end && *p == ',') {
            p = resend_json_skip_ws(p + 1, end);
            if (p >= end || *p == '}')
                return -1;
            continue;
        }
        if (p < end && *p == '}') {
            *next = p + 1;
            if (!key)
                return 0;
            if (!found)
                return 1;
            *val = found_val;
            *val_end = found_end;
            return 0;
        }
        return -1;
    }
}

static int skip_value(const char *p, const char *end, const char **next, int depth) {
    if (depth >= JSON_MAX_DEPTH)
        return -1;
    p = resend_json_skip_ws(p, end);
    if (p >= end)
        return -1;
    if (*p == '"')
        return skip_string(p, end, next);
    if (*p == '{')
        return walk_object(p, end, NULL, NULL, NULL, next, depth) == 0 ? 0 : -1;
    if (*p == '[')
        return skip_array(p, end, next, depth);
    if (*p == 't')
        return skip_lit(p, end, "true", next);
    if (*p == 'f')
        return skip_lit(p, end, "false", next);
    if (*p == 'n')
        return skip_lit(p, end, "null", next);
    if (*p == '-' || (*p >= '0' && *p <= '9'))
        return skip_number(p, end, next);
    return -1;
}

int resend_json_skip_value(const char *p, const char *end, const char **next) {
    return skip_value(p, end, next, 0);
}

int resend_json_array_step(const char **p, const char *end) {
    const char *q = resend_json_skip_ws(*p, end);
    if (q < end && *q == ',') {
        q = resend_json_skip_ws(q + 1, end);
        if (q >= end || *q == ']')
            return -1;
        *p = q;
        return 1;
    }
    if (q < end && *q == ']') {
        *p = q;
        return 0;
    }
    return -1;
}

/* 0 found, 1 absent, -1 malformed. The slice must be a single object. */
static int member_at(const char *json, size_t len, const char *key, const char **val,
                     const char **val_end) {
    if (!json || !key || !val || !val_end)
        return -1;
    const char *end = json + len;
    const char *p = resend_json_skip_ws(json, end);
    const char *next = NULL;
    int st = walk_object(p, end, key, val, val_end, &next, 0);
    if (st < 0 || !next)
        return -1;
    if (resend_json_skip_ws(next, end) != end)
        return -1;
    return st;
}

static int token_is(const char *p, const char *end, const char *lit) {
    if (!p || !end || p > end || !lit)
        return 0;
    size_t n = strlen(lit);
    if ((size_t)(end - p) < n || memcmp(p, lit, n) != 0)
        return 0;
    return resend_json_skip_ws(p + n, end) == end;
}

int resend_json_is_null(const char *p, const char *end) { return token_is(p, end, "null"); }

int resend_json_member(const char *json, size_t len, const char *key, const char **val,
                const char **val_end) {
    return member_at(json, len, key, val, val_end);
}

int resend_json_get_string(const char *json, size_t len, const char *key, char **out) {
    *out = NULL;
    const char *val = NULL;
    const char *val_end = NULL;
    if (member_at(json, len, key, &val, &val_end) != 0)
        return -1;
    if (val >= val_end || *val != '"')
        return -1;
    const char *next = NULL;
    return resend_json_parse_string(val, val_end, out, &next);
}

int resend_json_opt_string(const char *json, size_t len, const char *key, char **out) {
    *out = NULL;
    const char *val = NULL;
    const char *val_end = NULL;
    int st = member_at(json, len, key, &val, &val_end);
    if (st < 0)
        return -1;
    if (st > 0)
        return 0;
    val = resend_json_skip_ws(val, val_end);
    if (resend_json_is_null(val, val_end))
        return 0;
    if (val >= val_end || *val != '"')
        return -1;
    const char *next = NULL;
    return resend_json_parse_string(val, val_end, out, &next);
}

static void free_string_array(char **items, size_t count) {
    if (!items)
        return;
    for (size_t i = 0; i < count; i++)
        free(items[i]);
    free(items);
}

int resend_json_opt_string_array(const char *json, size_t len, const char *key, char ***out,
                          size_t *count) {
    *out = NULL;
    *count = 0;
    const char *val = NULL;
    const char *val_end = NULL;
    int st = member_at(json, len, key, &val, &val_end);
    if (st < 0)
        return -1;
    if (st > 0)
        return 0;
    val = resend_json_skip_ws(val, val_end);
    if (resend_json_is_null(val, val_end))
        return 0;
    if (val >= val_end || *val != '[')
        return -1;
    const char *p = val + 1;
    char **items = NULL;
    size_t n = 0;
    size_t cap = 0;
    for (;;) {
        p = resend_json_skip_ws(p, val_end);
        if (p < val_end && *p == ']')
            break;
        if (p >= val_end || *p != '"')
            goto fail;
        char *s = NULL;
        if (resend_json_parse_string(p, val_end, &s, &p) != 0)
            goto fail;
        if (n == cap) {
            size_t ncap = cap ? cap * 2 : 4;
            char **ni = realloc(items, ncap * sizeof *ni);
            if (!ni) {
                free(s);
                goto fail;
            }
            items = ni;
            cap = ncap;
        }
        items[n++] = s;
        int step = resend_json_array_step(&p, val_end);
        if (step < 0)
            goto fail;
        if (step == 0)
            break;
    }
    *out = items;
    *count = n;
    return 0;
fail:
    free_string_array(items, n);
    return -1;
}

int resend_json_opt_bool(const char *json, size_t len, const char *key, int *out) {
    *out = 0;
    const char *val = NULL;
    const char *val_end = NULL;
    int st = member_at(json, len, key, &val, &val_end);
    if (st < 0)
        return -1;
    if (st > 0)
        return 0;
    val = resend_json_skip_ws(val, val_end);
    if (resend_json_is_null(val, val_end))
        return 0;
    if (token_is(val, val_end, "true")) {
        *out = 1;
        return 0;
    }
    if (token_is(val, val_end, "false"))
        return 0;
    return -1;
}

static int parse_i64(const char *p, const char *end, int64_t *out) {
    p = resend_json_skip_ws(p, end);
    int neg = 0;
    if (p < end && *p == '-') {
        neg = 1;
        p++;
    }
    if (p >= end || *p < '0' || *p > '9')
        return -1;
    const char *digs = p;
    if (*p == '0')
        p++;
    else
        while (p < end && *p >= '0' && *p <= '9')
            p++;
    const char *digs_end = p;
    if (resend_json_skip_ws(p, end) != end)
        return -1;

    uint64_t mag = 0;
    for (const char *d = digs; d < digs_end; d++) {
        unsigned digit = (unsigned)(*d - '0');
        if (mag > (UINT64_MAX - digit) / 10)
            return -1;
        mag = mag * 10u + digit;
    }
    if (neg) {
        if (mag > (uint64_t)INT64_MAX + 1)
            return -1;
        *out = mag == (uint64_t)INT64_MAX + 1 ? INT64_MIN : -(int64_t)mag;
        return 0;
    }
    if (mag > (uint64_t)INT64_MAX)
        return -1;
    *out = (int64_t)mag;
    return 0;
}

int resend_json_opt_i64(const char *json, size_t len, const char *key, int64_t *out) {
    if (!out)
        return -1;
    *out = 0;
    const char *val = NULL;
    const char *val_end = NULL;
    int st = member_at(json, len, key, &val, &val_end);
    if (st < 0)
        return -1;
    if (st > 0)
        return 0;
    val = resend_json_skip_ws(val, val_end);
    if (resend_json_is_null(val, val_end))
        return 0;
    return parse_i64(val, val_end, out);
}

int resend_json_each_member(const char *json, size_t len,
                     int (*fn)(const char *key, const char *val, const char *val_end, void *ctx),
                     void *ctx) {
    if (!json || !fn)
        return -1;
    const char *end = json + len;
    const char *p = resend_json_skip_ws(json, end);
    if (p >= end || *p != '{')
        return -1;
    p++;
    p = resend_json_skip_ws(p, end);
    if (p < end && *p == '}') {
        if (resend_json_skip_ws(p + 1, end) != end)
            return -1;
        return 0;
    }
    for (;;) {
        char *key = NULL;
        if (resend_json_parse_string(p, end, &key, &p) != 0)
            return -1;
        p = resend_json_skip_ws(p, end);
        if (p >= end || *p != ':') {
            free(key);
            return -1;
        }
        p++;
        p = resend_json_skip_ws(p, end);
        const char *start = p;
        if (resend_json_skip_value(p, end, &p) != 0 || p == start) {
            free(key);
            return -1;
        }
        int rc = fn(key, start, p, ctx);
        free(key);
        if (rc != 0)
            return rc;
        p = resend_json_skip_ws(p, end);
        if (p < end && *p == ',') {
            p = resend_json_skip_ws(p + 1, end);
            if (p >= end || *p == '}')
                return -1;
            continue;
        }
        if (p < end && *p == '}') {
            if (resend_json_skip_ws(p + 1, end) != end)
                return -1;
            return 0;
        }
        return -1;
    }
}
