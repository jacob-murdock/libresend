#include "internal.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

void resend_buf_init(resend_buf *b) {
    b->data = NULL;
    b->len = 0;
    b->cap = 0;
}

void resend_buf_free(resend_buf *b) {
    free(b->data);
    b->data = NULL;
    b->len = 0;
    b->cap = 0;
}

int resend_buf_reserve(resend_buf *b, size_t need) {
    if (need <= b->cap)
        return 0;
    size_t cap = b->cap ? b->cap : 64;
    while (cap < need) {
        if (cap > (SIZE_MAX / 2)) {
            cap = need;
            break;
        }
        cap *= 2;
    }
    char *grown = realloc(b->data, cap);
    if (!grown)
        return -1;
    b->data = grown;
    b->cap = cap;
    return 0;
}

int resend_buf_putc(resend_buf *b, char c) {
    if (resend_buf_reserve(b, b->len + 2) != 0)
        return -1;
    b->data[b->len++] = c;
    b->data[b->len] = '\0';
    return 0;
}

int resend_buf_put(resend_buf *b, const char *s, size_t n) {
    if (resend_buf_reserve(b, b->len + n + 1) != 0)
        return -1;
    memcpy(b->data + b->len, s, n);
    b->len += n;
    b->data[b->len] = '\0';
    return 0;
}

int resend_buf_puts(resend_buf *b, const char *s) {
    return resend_buf_put(b, s, strlen(s));
}

int resend_ascii_ncasecmp(const char *a, const char *b, size_t n) {
    for (size_t i = 0; i < n; i++) {
        unsigned char ca = (unsigned char)a[i];
        unsigned char cb = (unsigned char)b[i];
        if (ca >= 'A' && ca <= 'Z')
            ca = (unsigned char)(ca + ('a' - 'A'));
        if (cb >= 'A' && cb <= 'Z')
            cb = (unsigned char)(cb + ('a' - 'A'));
        if (ca != cb || ca == 0)
            return (int)ca - (int)cb;
    }
    return 0;
}
