#include "internal.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int json_object_begin(resend_buf *b) { return resend_buf_putc(b, '{'); }
int json_object_end(resend_buf *b) { return resend_buf_putc(b, '}'); }
int json_array_begin(resend_buf *b) { return resend_buf_putc(b, '['); }
int json_array_end(resend_buf *b) { return resend_buf_putc(b, ']'); }
int json_comma(resend_buf *b) { return resend_buf_putc(b, ','); }

static int json_escape_put(resend_buf *b, unsigned char c) {
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

int json_string(resend_buf *b, const char *s) {
    if (!s)
        s = "";
    if (resend_buf_putc(b, '"') != 0)
        return -1;
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        if (json_escape_put(b, *p) != 0)
            return -1;
    }
    return resend_buf_putc(b, '"');
}

int json_key(resend_buf *b, const char *key) {
    if (json_string(b, key) != 0)
        return -1;
    return resend_buf_putc(b, ':');
}

int json_string_array(resend_buf *b, const char *const *items, size_t count) {
    if (json_array_begin(b) != 0)
        return -1;
    for (size_t i = 0; i < count; i++) {
        if (i && json_comma(b) != 0)
            return -1;
        if (json_string(b, items[i]) != 0)
            return -1;
    }
    return json_array_end(b);
}

int json_pair_object(resend_buf *b, const resend_pair *pairs, size_t count) {
    if (json_object_begin(b) != 0)
        return -1;
    for (size_t i = 0; i < count; i++) {
        if (i && json_comma(b) != 0)
            return -1;
        if (json_key(b, pairs[i].name) != 0 || json_string(b, pairs[i].value) != 0)
            return -1;
    }
    return json_object_end(b);
}

int json_tag_array(resend_buf *b, const resend_pair *tags, size_t count) {
    if (json_array_begin(b) != 0)
        return -1;
    for (size_t i = 0; i < count; i++) {
        if (i && json_comma(b) != 0)
            return -1;
        if (json_object_begin(b) != 0)
            return -1;
        if (json_key(b, "name") != 0 || json_string(b, tags[i].name) != 0)
            return -1;
        if (json_comma(b) != 0)
            return -1;
        if (json_key(b, "value") != 0 || json_string(b, tags[i].value) != 0)
            return -1;
        if (json_object_end(b) != 0)
            return -1;
    }
    return json_array_end(b);
}

static const char *skip_ws(const char *p, const char *end) {
    while (p < end && isspace((unsigned char)*p))
        p++;
    return p;
}

static int parse_string(const char *p, const char *end, char **out, const char **next) {
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
            case 'u': {
                if (p + 4 > end) {
                    resend_buf_free(&b);
                    return -1;
                }
                unsigned int cp = 0;
                for (int i = 0; i < 4; i++) {
                    unsigned char h = (unsigned char)*p++;
                    cp <<= 4;
                    if (h >= '0' && h <= '9')
                        cp |= (unsigned int)(h - '0');
                    else if (h >= 'a' && h <= 'f')
                        cp |= (unsigned int)(h - 'a' + 10);
                    else if (h >= 'A' && h <= 'F')
                        cp |= (unsigned int)(h - 'A' + 10);
                    else {
                        resend_buf_free(&b);
                        return -1;
                    }
                }
                if (cp < 0x80) {
                    c = (unsigned char)cp;
                } else if (cp < 0x800) {
                    if (resend_buf_putc(&b, (char)(0xc0 | (cp >> 6))) != 0 ||
                        resend_buf_putc(&b, (char)(0x80 | (cp & 0x3f))) != 0) {
                        resend_buf_free(&b);
                        return -1;
                    }
                    continue;
                } else {
                    if (resend_buf_putc(&b, (char)(0xe0 | (cp >> 12))) != 0 ||
                        resend_buf_putc(&b, (char)(0x80 | ((cp >> 6) & 0x3f))) != 0 ||
                        resend_buf_putc(&b, (char)(0x80 | (cp & 0x3f))) != 0) {
                        resend_buf_free(&b);
                        return -1;
                    }
                    continue;
                }
                break;
            }
            default:
                resend_buf_free(&b);
                return -1;
            }
        }
        if (resend_buf_putc(&b, (char)c) != 0) {
            resend_buf_free(&b);
            return -1;
        }
    }
    resend_buf_free(&b);
    return -1;
}

static int skip_value(const char *p, const char *end, const char **next);

static int skip_string(const char *p, const char *end, const char **next) {
    char *tmp = NULL;
    if (parse_string(p, end, &tmp, next) != 0)
        return -1;
    free(tmp);
    return 0;
}

static int skip_object_or_array(const char *p, const char *end, char open, char close,
                                const char **next) {
    if (p >= end || *p != open)
        return -1;
    p++;
    int depth = 1;
    while (p < end && depth) {
        if (*p == '"') {
            if (skip_string(p, end, &p) != 0)
                return -1;
            continue;
        }
        if (*p == open)
            depth++;
        else if (*p == close)
            depth--;
        p++;
    }
    if (depth != 0)
        return -1;
    *next = p;
    return 0;
}

static int skip_value(const char *p, const char *end, const char **next) {
    p = skip_ws(p, end);
    if (p >= end)
        return -1;
    if (*p == '"')
        return skip_string(p, end, next);
    if (*p == '{')
        return skip_object_or_array(p, end, '{', '}', next);
    if (*p == '[')
        return skip_object_or_array(p, end, '[', ']', next);
    if (*p == 't' || *p == 'f' || *p == 'n' || *p == '-' || (*p >= '0' && *p <= '9')) {
        while (p < end && !isspace((unsigned char)*p) && *p != ',' && *p != '}' && *p != ']')
            p++;
        *next = p;
        return 0;
    }
    return -1;
}

int json_get_string(const char *json, size_t len, const char *key, char **out) {
    *out = NULL;
    if (!json || !key)
        return -1;
    const char *p = json;
    const char *end = json + len;
    p = skip_ws(p, end);
    if (p >= end || *p != '{')
        return -1;
    p++;
    for (;;) {
        p = skip_ws(p, end);
        if (p < end && *p == '}')
            return -1;
        char *k = NULL;
        if (parse_string(p, end, &k, &p) != 0)
            return -1;
        p = skip_ws(p, end);
        if (p >= end || *p != ':') {
            free(k);
            return -1;
        }
        p++;
        p = skip_ws(p, end);
        int want = strcmp(k, key) == 0;
        free(k);
        if (want) {
            if (p >= end || *p != '"')
                return -1;
            return parse_string(p, end, out, &p);
        }
        if (skip_value(p, end, &p) != 0)
            return -1;
        p = skip_ws(p, end);
        if (p < end && *p == ',') {
            p++;
            continue;
        }
        if (p < end && *p == '}')
            return -1;
        return -1;
    }
}
