#ifndef LIBRESEND_INTERNAL_H
#define LIBRESEND_INTERNAL_H

#include "resend.h"

#include <stddef.h>

#define LIBRESEND_VERSION "0.1.0"
#define LIBRESEND_DEFAULT_BASE "https://api.resend.com"
#define LIBRESEND_TIMEOUT_SEC 30
#define LIBRESEND_CONNECT_TIMEOUT_SEC 2
#define LIBRESEND_BODY_MAX (1024 * 1024)
#define LIBRESEND_LARGE_BODY_MAX ((size_t)64 * 1024 * 1024)

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

/* Reader primitives. A slice is one JSON value; unknown object fields are skipped.
 * Malformed JSON is -1. Nesting deeper than 64 is malformed. */
const char *json_skip_ws(const char *p, const char *end);
int json_parse_string(const char *p, const char *end, char **out, const char **next);
int json_skip_value(const char *p, const char *end, const char **next);
/* After an array element: 1 if another follows (*p at that value), 0 at ']', -1 if malformed. */
int json_array_step(const char **p, const char *end);
/* True for a null token, with optional trailing whitespace. */
int json_is_null(const char *p, const char *end);
/* Missing or JSON null -> *out NULL and 0. Wrong type or malformed -> -1. */
int json_opt_string(const char *json, size_t len, const char *key, char **out);
/* Missing or null -> empty array (*out NULL, *count 0). Wrong type or malformed -> -1. */
int json_opt_string_array(const char *json, size_t len, const char *key, char ***out, size_t *count);
/* Missing or null -> *out 0. Wrong type or malformed -> -1. */
int json_opt_bool(const char *json, size_t len, const char *key, int *out);
/* Missing or null -> *out 0. Non-integer, out of int64 range, or malformed -> -1. */
int json_opt_i64(const char *json, size_t len, const char *key, int64_t *out);
/*
 * Call fn for each member of one object, in order. key is valid only for that
 * call. A non-zero return from fn stops the walk and is returned as-is.
 * The slice is one object; trailing non-space is malformed.
 */
int json_each_member(const char *json, size_t len,
                     int (*fn)(const char *key, const char *val, const char *val_end, void *ctx),
                     void *ctx);
/* 0 found, 1 absent, -1 malformed. *val..*val_end is the raw value span. */
int json_member(const char *json, size_t len, const char *key, const char **val, const char **val_end);

typedef struct {
    long status; /* 0 when no HTTP response */
    resend_buf body;
} resend_http_response;

void resend_http_response_free(resend_http_response *r);

/*
 * One easy handle. System CA store. POST when body is non-NULL.
 * idempotency_key, when non-NULL, is sent as Idempotency-Key.
 * batch_validation, when non-NULL, is sent as x-batch-validation.
 * On CURLE failure, fills the client error slot (status 0) and returns -1.
 */
int resend_http_json(resend *client, const char *method, const char *path, const char *body,
                     const char *idempotency_key, resend_http_response *out);
int resend_http_json_limit(resend *client, const char *method, const char *path, const char *body,
                           const char *idempotency_key, const char *batch_validation,
                           size_t body_max, resend_http_response *out);

/*
 * prefix is "/emails/" or "/emails/receiving/". List and get fill download_url
 * and expires_at. Failure sets the error slot.
 */
int resend_attachments_list(resend *client, const char *prefix, const char *email_id,
                            const resend_page *page, resend_received_attachment_list *out);
int resend_attachment_get(resend *client, const char *prefix, const char *email_id,
                          const char *attachment_id, resend_received_attachment *out);

/*
 * Build path with optional page query. Rejects after+before together (error slot,
 * no request). limit 0 omits the parameter. Query values are percent-encoded.
 * *out is malloc'd.
 */
int resend_path_with_page(resend *client, const char *prefix, const resend_page *page, char **out);

/*
 * prefix + one percent-encoded path segment + optional suffix (NULL omits it).
 * Rejects an id that is `.` or `..` (and %2e forms). *out is malloc'd.
 * Failure sets the error slot.
 */
int resend_path_with_id(resend *client, const char *prefix, const char *id, const char *suffix,
                        char **out);

#endif /* LIBRESEND_INTERNAL_H */
