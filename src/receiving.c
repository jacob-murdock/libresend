#include "internal.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static void free_strings(char **items, size_t count) {
    if (!items)
        return;
    for (size_t i = 0; i < count; i++)
        free(items[i]);
    free(items);
}

static void free_headers(resend_header *items, size_t count) {
    if (!items)
        return;
    for (size_t i = 0; i < count; i++) {
        free(items[i].name);
        free(items[i].value);
    }
    free(items);
}

void resend_received_attachment_free(resend_received_attachment *attachment) {
    if (!attachment)
        return;
    free(attachment->id);
    free(attachment->filename);
    free(attachment->content_type);
    free(attachment->content_disposition);
    free(attachment->content_id);
    free(attachment->download_url);
    free(attachment->expires_at);
    memset(attachment, 0, sizeof *attachment);
}

void resend_received_free(resend_received *email) {
    if (!email)
        return;
    free(email->id);
    free(email->from);
    free_strings(email->to, email->to_count);
    free_strings(email->cc, email->cc_count);
    free_strings(email->bcc, email->bcc_count);
    free_strings(email->reply_to, email->reply_to_count);
    free_strings(email->received_for, email->received_for_count);
    free(email->subject);
    free(email->message_id);
    free(email->html);
    free(email->html_format);
    free(email->text);
    free_headers(email->headers, email->header_count);
    free(email->created_at);
    free(email->authentication.spf);
    free(email->authentication.dkim);
    free(email->authentication.dmarc);
    free(email->raw.download_url);
    free(email->raw.expires_at);
    if (email->attachments) {
        for (size_t i = 0; i < email->attachment_count; i++)
            resend_received_attachment_free(&email->attachments[i]);
        free(email->attachments);
    }
    memset(email, 0, sizeof *email);
}

void resend_received_list_free(resend_received_list *list) {
    if (!list)
        return;
    if (list->items) {
        for (size_t i = 0; i < list->count; i++)
            resend_received_free(&list->items[i]);
        free(list->items);
    }
    memset(list, 0, sizeof *list);
}

void resend_received_attachment_list_free(resend_received_attachment_list *list) {
    if (!list)
        return;
    if (list->items) {
        for (size_t i = 0; i < list->count; i++)
            resend_received_attachment_free(&list->items[i]);
        free(list->items);
    }
    memset(list, 0, sizeof *list);
}

typedef struct {
    resend_header *items;
    size_t n;
    size_t cap;
} header_acc;

static int take_header(const char *key, const char *val, const char *val_end, void *ctx) {
    header_acc *acc = ctx;
    val = resend_json_skip_ws(val, val_end);
    if (val >= val_end || *val != '"')
        return -1;
    char *value = NULL;
    const char *next = NULL;
    if (resend_json_parse_string(val, val_end, &value, &next) != 0)
        return -1;
    if (resend_json_skip_ws(next, val_end) != val_end) {
        free(value);
        return -1;
    }
    char *name = strdup(key);
    if (!name) {
        free(value);
        return -1;
    }
    if (acc->n == acc->cap) {
        size_t ncap = acc->cap ? acc->cap * 2 : 4;
        resend_header *ni = realloc(acc->items, ncap * sizeof *ni);
        if (!ni) {
            free(name);
            free(value);
            return -1;
        }
        acc->items = ni;
        acc->cap = ncap;
    }
    acc->items[acc->n].name = name;
    acc->items[acc->n].value = value;
    acc->n++;
    return 0;
}

/* 0 object (*val at '{', *n its length), 1 missing or null, -1 otherwise. */
static int object_member(const char *json, size_t len, const char *key, const char **val,
                         size_t *n) {
    const char *end = NULL;
    int st = resend_json_member(json, len, key, val, &end);
    if (st != 0)
        return st;
    const char *p = resend_json_skip_ws(*val, end);
    if (resend_json_is_null(p, end))
        return 1;
    if (p >= end || *p != '{')
        return -1;
    *val = p;
    *n = (size_t)(end - p);
    return 0;
}

static int parse_headers(const char *json, size_t len, resend_received *out) {
    const char *val = NULL;
    size_t n = 0;
    int st = object_member(json, len, "headers", &val, &n);
    if (st != 0)
        return st < 0 ? -1 : 0;
    header_acc acc = {0};
    if (resend_json_each_member(val, n, take_header, &acc) != 0) {
        free_headers(acc.items, acc.n);
        return -1;
    }
    out->headers = acc.items;
    out->header_count = acc.n;
    return 0;
}

static int parse_authentication(const char *json, size_t len, resend_authentication *out) {
    const char *val = NULL;
    size_t n = 0;
    int st = object_member(json, len, "authentication", &val, &n);
    if (st != 0)
        return st < 0 ? -1 : 0;
    if (resend_json_opt_string(val, n, "spf", &out->spf) != 0 ||
        resend_json_opt_string(val, n, "dkim", &out->dkim) != 0 ||
        resend_json_opt_string(val, n, "dmarc", &out->dmarc) != 0)
        return -1;
    return 0;
}

static int parse_raw(const char *json, size_t len, resend_raw *out) {
    const char *val = NULL;
    size_t n = 0;
    int st = object_member(json, len, "raw", &val, &n);
    if (st != 0)
        return st < 0 ? -1 : 0;
    if (resend_json_opt_string(val, n, "download_url", &out->download_url) != 0 ||
        resend_json_opt_string(val, n, "expires_at", &out->expires_at) != 0)
        return -1;
    return 0;
}

/*
 * Email payloads carry attachment metadata only (with_urls 0). Attachment
 * list/get set download_url and expires_at (with_urls 1).
 */
static int parse_attachment(const char *json, size_t len, resend_received_attachment *out,
                            int with_urls) {
    memset(out, 0, sizeof *out);
    if (resend_json_opt_string(json, len, "id", &out->id) != 0 ||
        resend_json_opt_string(json, len, "filename", &out->filename) != 0 ||
        resend_json_opt_string(json, len, "content_type", &out->content_type) != 0 ||
        resend_json_opt_string(json, len, "content_disposition", &out->content_disposition) != 0 ||
        resend_json_opt_string(json, len, "content_id", &out->content_id) != 0 ||
        resend_json_opt_i64(json, len, "size", &out->size) != 0 || !out->id) {
        resend_received_attachment_free(out);
        return -1;
    }
    if (with_urls) {
        if (resend_json_opt_string(json, len, "download_url", &out->download_url) != 0 ||
            resend_json_opt_string(json, len, "expires_at", &out->expires_at) != 0) {
            resend_received_attachment_free(out);
            return -1;
        }
    }
    return 0;
}

typedef int (*parse_item_fn)(const char *json, size_t len, void *slot);
typedef void (*free_item_fn)(void *slot);

/* Missing or null key is an empty array. */
static int parse_object_array(const char *json, size_t len, const char *key, size_t item_size,
                              parse_item_fn parse, free_item_fn free_item, void **out,
                              size_t *count) {
    *out = NULL;
    *count = 0;
    const char *val = NULL;
    const char *val_end = NULL;
    int found = resend_json_member(json, len, key, &val, &val_end);
    if (found < 0)
        return -1;
    if (found > 0)
        return 0;
    val = resend_json_skip_ws(val, val_end);
    if (resend_json_is_null(val, val_end))
        return 0;
    if (val >= val_end || *val != '[')
        return -1;

    const char *p = val + 1;
    char *items = NULL;
    size_t n = 0;
    size_t cap = 0;
    for (;;) {
        p = resend_json_skip_ws(p, val_end);
        if (p < val_end && *p == ']')
            break;
        if (p >= val_end || *p != '{')
            goto fail;
        const char *obj = p;
        if (resend_json_skip_value(p, val_end, &p) != 0 || p == obj)
            goto fail;
        if (n == cap) {
            if (cap && cap > SIZE_MAX / 2)
                goto fail;
            size_t ncap = cap ? cap * 2 : 4;
            if (item_size == 0 || ncap > SIZE_MAX / item_size)
                goto fail;
            char *ni = realloc(items, ncap * item_size);
            if (!ni)
                goto fail;
            items = ni;
            cap = ncap;
        }
        if (parse(obj, (size_t)(p - obj), items + n * item_size) != 0)
            goto fail;
        n++;
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
    if (items) {
        for (size_t i = 0; i < n; i++)
            free_item(items + i * item_size);
        free(items);
    }
    return -1;
}

static int parse_att_meta(const char *json, size_t len, void *slot) {
    return parse_attachment(json, len, slot, 0);
}

static int parse_att_urls(const char *json, size_t len, void *slot) {
    return parse_attachment(json, len, slot, 1);
}

static void free_att(void *slot) {
    resend_received_attachment_free(slot);
}

static int parse_attachments(const char *json, size_t len, resend_received_attachment **out,
                             size_t *count) {
    return parse_object_array(json, len, "attachments", sizeof(resend_received_attachment),
                              parse_att_meta, free_att, (void **)out, count);
}

static int parse_attachment_list(const char *json, size_t len,
                                 resend_received_attachment_list *out) {
    memset(out, 0, sizeof *out);
    if (resend_json_opt_bool(json, len, "has_more", &out->has_more) != 0)
        return -1;
    return parse_object_array(json, len, "data", sizeof(resend_received_attachment), parse_att_urls,
                              free_att, (void **)&out->items, &out->count);
}

static int parse_received_envelope(const char *json, size_t len, resend_received *out) {
    memset(out, 0, sizeof *out);
    if (resend_json_opt_string(json, len, "id", &out->id) != 0 ||
        resend_json_opt_string(json, len, "from", &out->from) != 0 ||
        resend_json_opt_string(json, len, "subject", &out->subject) != 0 ||
        resend_json_opt_string(json, len, "message_id", &out->message_id) != 0 ||
        resend_json_opt_string(json, len, "created_at", &out->created_at) != 0 ||
        resend_json_opt_string_array(json, len, "to", &out->to, &out->to_count) != 0 ||
        resend_json_opt_string_array(json, len, "cc", &out->cc, &out->cc_count) != 0 ||
        resend_json_opt_string_array(json, len, "bcc", &out->bcc, &out->bcc_count) != 0 ||
        resend_json_opt_string_array(json, len, "reply_to", &out->reply_to, &out->reply_to_count) != 0 ||
        parse_attachments(json, len, &out->attachments, &out->attachment_count) != 0 || !out->id) {
        resend_received_free(out);
        return -1;
    }
    return 0;
}

static int parse_received(const char *json, size_t len, resend_received *out) {
    if (parse_received_envelope(json, len, out) != 0)
        return -1;
    if (resend_json_opt_string(json, len, "html", &out->html) != 0 ||
        resend_json_opt_string(json, len, "html_format", &out->html_format) != 0 ||
        resend_json_opt_string(json, len, "text", &out->text) != 0 ||
        resend_json_opt_string_array(json, len, "received_for", &out->received_for,
                              &out->received_for_count) != 0 ||
        parse_headers(json, len, out) != 0 ||
        parse_authentication(json, len, &out->authentication) != 0 ||
        parse_raw(json, len, &out->raw) != 0) {
        resend_received_free(out);
        return -1;
    }
    return 0;
}

static int parse_env(const char *json, size_t len, void *slot) {
    return parse_received_envelope(json, len, slot);
}

static void free_recv(void *slot) {
    resend_received_free(slot);
}

static int parse_received_list(const char *json, size_t len, resend_received_list *out) {
    memset(out, 0, sizeof *out);
    if (resend_json_opt_bool(json, len, "has_more", &out->has_more) != 0)
        return -1;
    return parse_object_array(json, len, "data", sizeof(resend_received), parse_env, free_recv,
                              (void **)&out->items, &out->count);
}

/* Takes ownership of path. Non-2xx fills the error slot from the JSON body. */
static int receiving_get_ok(resend *client, char *path, size_t body_max,
                            resend_http_response *resp) {
    int rc = resend_http_json_limit(client, "GET", path, NULL, NULL, NULL, body_max, resp);
    free(path);
    if (rc != 0)
        return -1;
    if (resp->status < 200 || resp->status >= 300) {
        size_t len = resp->body.data ? resp->body.len : 0;
        resend_error_from_body(client, (int)resp->status, resp->body.data, len);
        resend_http_response_free(resp);
        return -1;
    }
    return 0;
}

int resend_receiving_get(resend *client, const char *email_id, const char *html_format,
                         resend_received *out) {
    if (!client)
        return -1;
    resend_error_clear(client);
    if (!email_id || !email_id[0]) {
        resend_error_set(client, 0, "missing_required_field", "missing id");
        return -1;
    }
    if (!out) {
        resend_error_set(client, 0, "missing_required_field", "missing result");
        return -1;
    }
    memset(out, 0, sizeof *out);

    const char *suffix = NULL;
    if (html_format) {
        if (strcmp(html_format, "data_uri") == 0)
            suffix = "?html_format=data_uri";
        else if (strcmp(html_format, "cid") == 0)
            suffix = "?html_format=cid";
        else {
            resend_error_set(client, 0, "validation_error", "html_format must be data_uri or cid");
            return -1;
        }
    }

    char *path = NULL;
    if (resend_path_with_id(client, "/emails/receiving/", email_id, suffix, &path) != 0)
        return -1;

    resend_http_response resp;
    if (receiving_get_ok(client, path, LIBRESEND_RECEIVING_BODY_MAX, &resp) != 0)
        return -1;

    size_t len = resp.body.data ? resp.body.len : 0;
    if (parse_received(resp.body.data ? resp.body.data : "", len, out) != 0) {
        resend_received_free(out);
        resend_error_set(client, (int)resp.status, NULL, "invalid received email");
        resend_http_response_free(&resp);
        return -1;
    }
    resend_http_response_free(&resp);
    return 0;
}

int resend_receiving_list(resend *client, const resend_page *page, resend_received_list *out) {
    if (!client)
        return -1;
    resend_error_clear(client);
    if (!out) {
        resend_error_set(client, 0, "missing_required_field", "missing result");
        return -1;
    }
    memset(out, 0, sizeof *out);

    char *path = NULL;
    if (resend_path_with_page(client, "/emails/receiving", page, &path) != 0)
        return -1;

    resend_http_response resp;
    if (receiving_get_ok(client, path, LIBRESEND_BODY_MAX, &resp) != 0)
        return -1;

    size_t len = resp.body.data ? resp.body.len : 0;
    if (parse_received_list(resp.body.data ? resp.body.data : "", len, out) != 0) {
        resend_received_list_free(out);
        resend_error_set(client, (int)resp.status, NULL, "invalid received email list");
        resend_http_response_free(&resp);
        return -1;
    }
    resend_http_response_free(&resp);
    return 0;
}

int resend_attachments_list(resend *client, const char *prefix, const char *email_id,
                            const resend_page *page, resend_received_attachment_list *out) {
    if (!client)
        return -1;
    resend_error_clear(client);
    if (!prefix || !email_id || !email_id[0]) {
        resend_error_set(client, 0, "missing_required_field", "missing id");
        return -1;
    }
    if (!out) {
        resend_error_set(client, 0, "missing_required_field", "missing result");
        return -1;
    }
    memset(out, 0, sizeof *out);

    char *base = NULL;
    if (resend_path_with_id(client, prefix, email_id, "/attachments", &base) != 0)
        return -1;

    char *path = NULL;
    if (resend_path_with_page(client, base, page, &path) != 0) {
        free(base);
        return -1;
    }
    free(base);

    resend_http_response resp;
    if (receiving_get_ok(client, path, LIBRESEND_BODY_MAX, &resp) != 0)
        return -1;

    size_t len = resp.body.data ? resp.body.len : 0;
    if (parse_attachment_list(resp.body.data ? resp.body.data : "", len, out) != 0) {
        resend_received_attachment_list_free(out);
        resend_error_set(client, (int)resp.status, NULL, "invalid attachment list");
        resend_http_response_free(&resp);
        return -1;
    }
    resend_http_response_free(&resp);
    return 0;
}

int resend_attachment_get(resend *client, const char *prefix, const char *email_id,
                          const char *attachment_id, resend_received_attachment *out) {
    if (!client)
        return -1;
    resend_error_clear(client);
    if (!prefix || !email_id || !email_id[0] || !attachment_id || !attachment_id[0]) {
        resend_error_set(client, 0, "missing_required_field", "missing id");
        return -1;
    }
    if (!out) {
        resend_error_set(client, 0, "missing_required_field", "missing result");
        return -1;
    }
    memset(out, 0, sizeof *out);

    char *built = NULL;
    if (resend_path_with_id(client, prefix, email_id, "/attachments/", &built) != 0)
        return -1;

    char *path = NULL;
    if (resend_path_with_id(client, built, attachment_id, NULL, &path) != 0) {
        free(built);
        return -1;
    }
    free(built);

    resend_http_response resp;
    if (receiving_get_ok(client, path, LIBRESEND_BODY_MAX, &resp) != 0)
        return -1;

    size_t len = resp.body.data ? resp.body.len : 0;
    if (parse_attachment(resp.body.data ? resp.body.data : "", len, out, 1) != 0) {
        resend_received_attachment_free(out);
        resend_error_set(client, (int)resp.status, NULL, "invalid attachment");
        resend_http_response_free(&resp);
        return -1;
    }
    resend_http_response_free(&resp);
    return 0;
}

int resend_receiving_attachments_list(resend *client, const char *email_id, const resend_page *page,
                                      resend_received_attachment_list *out) {
    return resend_attachments_list(client, "/emails/receiving/", email_id, page, out);
}

int resend_receiving_attachment_get(resend *client, const char *email_id, const char *attachment_id,
                                    resend_received_attachment *out) {
    return resend_attachment_get(client, "/emails/receiving/", email_id, attachment_id, out);
}
