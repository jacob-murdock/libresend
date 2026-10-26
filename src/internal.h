#ifndef LIBRESEND_INTERNAL_H
#define LIBRESEND_INTERNAL_H

#include "resend.h"

#include <stddef.h>

#define LIBRESEND_VERSION "0.1.0"
#define LIBRESEND_DEFAULT_BASE "https://api.resend.com"
#define LIBRESEND_TIMEOUT_SEC 30
#define LIBRESEND_CONNECT_TIMEOUT_SEC 2
#define LIBRESEND_BODY_MAX (1024 * 1024)

struct resend {
    char *api_key;
    char *base_url;
    resend_error err;
    char *err_name;
    char *err_message;
};

void resend_error_clear(resend *client);
void resend_error_set(resend *client, int status, const char *name, const char *message);
/* Non-2xx: HTTP status plus optional top-level name and message strings. */
void resend_error_from_body(resend *client, int status, const char *body, size_t len);

typedef struct {
    char *data;
    size_t len;
    size_t cap;
} resend_buf;

void resend_buf_init(resend_buf *b);
void resend_buf_free(resend_buf *b);
int resend_buf_reserve(resend_buf *b, size_t need);
int resend_buf_putc(resend_buf *b, char c);
int resend_buf_put(resend_buf *b, const char *s, size_t n);
int resend_buf_puts(resend_buf *b, const char *s);

/* JSON writer. Keys and string values are escaped. */
int json_object_begin(resend_buf *b);
int json_object_end(resend_buf *b);
int json_array_begin(resend_buf *b);
int json_array_end(resend_buf *b);
int json_comma(resend_buf *b);
int json_key(resend_buf *b, const char *key);
int json_string(resend_buf *b, const char *s);
int json_string_array(resend_buf *b, const char *const *items, size_t count);
/* Object of string pairs: {"k":"v",...}. */
int json_pair_object(resend_buf *b, const resend_pair *pairs, size_t count);
/* Array of {name,value} objects for tags. */
int json_tag_array(resend_buf *b, const resend_pair *tags, size_t count);

/* First top-level string field named key. *out is malloc'd. */
int json_get_string(const char *json, size_t len, const char *key, char **out);

typedef struct {
    long status; /* 0 when no HTTP response */
    resend_buf body;
} resend_http_response;

void resend_http_response_free(resend_http_response *r);

/*
 * One easy handle. System CA store. POST when body is non-NULL.
 * idempotency_key, when non-NULL, is sent as Idempotency-Key.
 * On CURLE failure, fills the client error slot (status 0) and returns -1.
 */
int resend_http_json(resend *client, const char *method, const char *path, const char *body,
                     const char *idempotency_key, resend_http_response *out);

#endif /* LIBRESEND_INTERNAL_H */
