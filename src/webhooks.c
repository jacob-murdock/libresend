#include "internal.h"

#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>

#include <limits.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

enum { WEBHOOK_TOLERANCE_SEC = 300, WEBHOOK_TIMESTAMP_DIGITS = 20 };

static void webhook_err(resend_error *err, const char *message) {
    if (!err)
        return;
    err->status = 0;
    err->name = NULL;
    err->message = message && message[0] ? message : "";
}

static int b64_char(unsigned char c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '+' ||
           c == '/';
}

/* Standard base64, with or without padding. EVP_DecodeBlock keeps a byte per '='. */
static int b64_decode(const char *in, unsigned char **out, size_t *out_len) {
    size_t in_len = strlen(in);
    if (in_len == 0 || in_len > (size_t)INT_MAX - 4)
        return -1;

    size_t i = 0;
    while (i < in_len && in[i] != '=') {
        if (!b64_char((unsigned char)in[i]))
            return -1;
        i++;
    }
    size_t eq = in_len - i;
    if (eq > 2)
        return -1;
    for (size_t j = i; j < in_len; j++) {
        if (in[j] != '=')
            return -1;
    }
    if (eq) {
        if (in_len % 4 != 0)
            return -1;
    } else if (in_len % 4 == 1) {
        return -1;
    }

    size_t pad_added = (4 - (in_len % 4)) % 4;
    size_t total = in_len + pad_added;
    char *padded = malloc(total + 1);
    if (!padded)
        return -1;
    memcpy(padded, in, in_len);
    memset(padded + in_len, '=', pad_added);
    padded[total] = '\0';

    size_t cap = (total / 4) * 3;
    unsigned char *buf = malloc(cap ? cap : 1);
    if (!buf) {
        free(padded);
        return -1;
    }
    int n = EVP_DecodeBlock(buf, (unsigned char *)padded, (int)total);
    free(padded);
    if (n < (int)(eq + pad_added)) {
        free(buf);
        return -1;
    }
    n -= (int)(eq + pad_added);
    if (n <= 0) {
        free(buf);
        return -1;
    }
    *out = buf;
    *out_len = (size_t)n;
    return 0;
}

static void free_key(unsigned char *key, size_t key_len) {
    if (!key)
        return;
    OPENSSL_cleanse(key, key_len);
    free(key);
}

static int b64_encode(const unsigned char *in, size_t in_len, char **out) {
    size_t cap = ((in_len + 2) / 3) * 4 + 1;
    char *buf = malloc(cap);
    if (!buf)
        return -1;
    EVP_EncodeBlock((unsigned char *)buf, in, (int)in_len);
    *out = buf;
    return 0;
}

/* Constant-time equality of two byte strings. Length mismatch is a miss. */
static int ct_equal(const char *a, size_t alen, const char *b, size_t blen) {
    if (alen != blen)
        return 0;
    if (alen == 0)
        return 1;
    return CRYPTO_memcmp(a, b, alen) == 0;
}

/* Decimal unix seconds. A value that does not fit in long is not a timestamp. */
static int parse_timestamp(const char *s, long *out) {
    if (!s || !s[0])
        return -1;
    size_t n = 0;
    unsigned long v = 0;
    for (const unsigned char *p = (const unsigned char *)s; *p; p++, n++) {
        if (n >= WEBHOOK_TIMESTAMP_DIGITS || *p < '0' || *p > '9')
            return -1;
        unsigned digit = (unsigned)(*p - '0');
        if (v > ((unsigned long)LONG_MAX - digit) / 10)
            return -1;
        v = v * 10 + digit;
    }
    *out = (long)v;
    return 0;
}

static int secret_key(const char *secret, unsigned char **key, size_t *key_len) {
    if (!secret || !secret[0])
        return -1;
    if (strncmp(secret, "whsec_", 6) == 0)
        secret += 6;
    if (!secret[0])
        return -1;
    return b64_decode(secret, key, key_len);
}

/* 0 ok, -1 out of memory, -2 length or HMAC failure. */
static int expected_sig(const unsigned char *key, size_t key_len, const char *svix_id,
                        const char *svix_timestamp, const void *body, size_t body_len,
                        char **out_b64) {
    if (key_len == 0 || key_len > (size_t)INT_MAX)
        return -2;
    size_t id_len = strlen(svix_id);
    size_t ts_len = strlen(svix_timestamp);
    if (id_len > SIZE_MAX - 2 || ts_len > SIZE_MAX - 2 - id_len ||
        body_len > SIZE_MAX - 2 - id_len - ts_len)
        return -2;
    size_t signed_len = id_len + 1 + ts_len + 1 + body_len;
    unsigned char *signed_content = malloc(signed_len);
    if (!signed_content)
        return -1;
    memcpy(signed_content, svix_id, id_len);
    signed_content[id_len] = '.';
    memcpy(signed_content + id_len + 1, svix_timestamp, ts_len);
    signed_content[id_len + 1 + ts_len] = '.';
    if (body_len)
        memcpy(signed_content + id_len + 1 + ts_len + 1, body, body_len);

    unsigned char md[EVP_MAX_MD_SIZE];
    unsigned int md_len = 0;
    unsigned char *ok =
        HMAC(EVP_sha256(), key, (int)key_len, signed_content, signed_len, md, &md_len);
    free(signed_content);
    if (!ok || md_len != 32)
        return -2;
    return b64_encode(md, md_len, out_b64) == 0 ? 0 : -1;
}

/* True when any space-separated v1,<sig> matches expected (constant-time per value). */
static int signatures_match(const char *header, const char *expected) {
    if (!header || !expected)
        return 0;
    size_t expected_len = strlen(expected);
    int matched = 0;
    const char *p = header;
    while (*p) {
        while (*p == ' ')
            p++;
        if (!*p)
            break;
        const char *end = p;
        while (*end && *end != ' ')
            end++;
        if ((size_t)(end - p) > 3 && p[0] == 'v' && p[1] == '1' && p[2] == ',') {
            const char *sig = p + 3;
            size_t sig_len = (size_t)(end - sig);
            if (ct_equal(expected, expected_len, sig, sig_len))
                matched = 1;
        }
        p = end;
    }
    return matched;
}

static char *dup_span(const char *start, const char *end) {
    size_t n = (size_t)(end - start);
    char *s = malloc(n + 1);
    if (!s)
        return NULL;
    memcpy(s, start, n);
    s[n] = '\0';
    return s;
}

static int parse_event(const char *json, size_t len, resend_event *out) {
    memset(out, 0, sizeof *out);
    if (json_opt_string(json, len, "type", &out->type) != 0 || !out->type) {
        resend_event_free(out);
        return -1;
    }
    if (json_opt_string(json, len, "created_at", &out->created_at) != 0) {
        resend_event_free(out);
        return -1;
    }

    const char *val = NULL;
    const char *val_end = NULL;
    int found = json_member(json, len, "data", &val, &val_end);
    if (found < 0) {
        resend_event_free(out);
        return -1;
    }
    if (found == 0) {
        val = json_skip_ws(val, val_end);
        if (val < val_end && *val == '{') {
            const char *next = NULL;
            if (json_skip_value(val, val_end, &next) != 0) {
                resend_event_free(out);
                return -1;
            }
            out->data_json = dup_span(val, next);
            if (!out->data_json) {
                resend_event_free(out);
                return -1;
            }
            if (json_opt_string(out->data_json, (size_t)(next - val), "email_id",
                                &out->email_id) != 0) {
                resend_event_free(out);
                return -1;
            }
        } else if (!json_is_null(val, val_end)) {
            resend_event_free(out);
            return -1;
        }
    }
    return 0;
}

int resend_webhook_verify(const char *secret, const char *svix_id, const char *svix_timestamp,
                          const char *svix_signature, const void *body, size_t body_len,
                          resend_event *out, resend_error *err) {
    if (out)
        memset(out, 0, sizeof *out);
    if (!secret || !svix_id || !svix_id[0] || !svix_timestamp || !svix_timestamp[0] ||
        !svix_signature || !svix_signature[0] || !out || !err || (body_len > 0 && !body)) {
        webhook_err(err, "invalid argument");
        return -1;
    }
    webhook_err(err, "");

    long ts = 0;
    if (parse_timestamp(svix_timestamp, &ts) != 0) {
        webhook_err(err, "invalid timestamp");
        return -1;
    }
    time_t now_t = time(NULL);
    if (now_t < 0) {
        webhook_err(err, "timestamp outside tolerance");
        return -1;
    }
    /* Unsigned difference. A signed now - ts overflows when ts is near LONG_MAX. */
    unsigned long now = (unsigned long)now_t;
    unsigned long got = (unsigned long)ts;
    int stale = got > now ? (got - now > (unsigned long)WEBHOOK_TOLERANCE_SEC)
                          : (now - got > (unsigned long)WEBHOOK_TOLERANCE_SEC);
    if (stale) {
        webhook_err(err, "timestamp outside tolerance");
        return -1;
    }

    unsigned char *key = NULL;
    size_t key_len = 0;
    if (secret_key(secret, &key, &key_len) != 0) {
        webhook_err(err, "invalid secret");
        return -1;
    }

    char *expected = NULL;
    int sig_rc = expected_sig(key, key_len, svix_id, svix_timestamp, body, body_len, &expected);
    if (sig_rc != 0) {
        free_key(key, key_len);
        webhook_err(err, sig_rc == -1 ? "out of memory" : "failed to compute signature");
        return -1;
    }
    free_key(key, key_len);

    int ok = signatures_match(svix_signature, expected);
    OPENSSL_cleanse(expected, strlen(expected));
    free(expected);
    if (!ok) {
        webhook_err(err, "invalid signature");
        return -1;
    }

    if (parse_event(body ? (const char *)body : "", body_len, out) != 0) {
        webhook_err(err, "invalid payload");
        return -1;
    }
    return 0;
}

void resend_event_free(resend_event *event) {
    if (!event)
        return;
    free(event->type);
    free(event->created_at);
    free(event->email_id);
    free(event->data_json);
    memset(event, 0, sizeof *event);
}
