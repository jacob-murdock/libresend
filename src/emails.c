#include "internal.h"

#include <limits.h>
#include <openssl/evp.h>
#include <stdlib.h>
#include <string.h>

static int write_field(resend_buf *body, int *first, const char *key) {
    if (!*first && json_comma(body) != 0)
        return -1;
    *first = 0;
    return json_key(body, key);
}

static int write_string_field(resend_buf *body, int *first, const char *key, const char *value) {
    if (!value)
        return 0;
    if (write_field(body, first, key) != 0)
        return -1;
    return json_string(body, value);
}

static int write_string_array_field(resend_buf *body, int *first, const char *key,
                                   const char *const *items, size_t count) {
    if (count == 0)
        return 0;
    if (write_field(body, first, key) != 0)
        return -1;
    return json_string_array(body, items, count);
}

/* Exactly one of content or path. */
static int attachment_mode_ok(const resend_attachment *a) {
    int has_content = a->content != NULL;
    int has_path = a->path != NULL;
    return has_content != has_path;
}

static int reject_bad_attachments(resend *client, const resend_attachment *attachments,
                                  size_t count) {
    if (count > 0 && !attachments) {
        resend_error_set(client, 0, NULL, "missing attachments");
        return -1;
    }
    for (size_t i = 0; i < count; i++) {
        if (!attachment_mode_ok(&attachments[i])) {
            resend_error_set(client, 0, NULL, "attachment needs content or path, not both");
            return -1;
        }
    }
    return 0;
}

static char *base64_encode(const void *data, size_t len) {
    if (len > (size_t)INT_MAX)
        return NULL;
    size_t cap = 4 * ((len + 2) / 3) + 1;
    char *out = malloc(cap);
    if (!out)
        return NULL;
    int n = EVP_EncodeBlock((unsigned char *)out, (const unsigned char *)data, (int)len);
    if (n < 0) {
        free(out);
        return NULL;
    }
    out[n] = '\0';
    return out;
}

static int write_attachment(resend_buf *body, const resend_attachment *a) {
    if (json_object_begin(body) != 0)
        return -1;
    int first = 1;
    if (write_string_field(body, &first, "filename", a->filename) != 0)
        return -1;
    if (a->content) {
        char *b64 = base64_encode(a->content, a->content_len);
        if (!b64)
            return -1;
        int rc = write_string_field(body, &first, "content", b64);
        free(b64);
        if (rc != 0)
            return -1;
    } else if (write_string_field(body, &first, "path", a->path) != 0) {
        return -1;
    }
    if (write_string_field(body, &first, "content_type", a->content_type) != 0)
        return -1;
    if (write_string_field(body, &first, "content_id", a->content_id) != 0)
        return -1;
    return json_object_end(body);
}

static int write_attachments(resend_buf *body, const resend_attachment *attachments, size_t count) {
    if (json_array_begin(body) != 0)
        return -1;
    for (size_t i = 0; i < count; i++) {
        if (i && json_comma(body) != 0)
            return -1;
        if (write_attachment(body, &attachments[i]) != 0)
            return -1;
    }
    return json_array_end(body);
}

static int write_send_object(const resend_send *message, resend_buf *body) {
    if (json_object_begin(body) != 0)
        return -1;

    int first = 1;
    if (write_string_field(body, &first, "from", message->from) != 0)
        return -1;
    if (write_string_array_field(body, &first, "to", message->to, message->to_count) != 0)
        return -1;
    if (write_string_array_field(body, &first, "cc", message->cc, message->cc_count) != 0)
        return -1;
    if (write_string_array_field(body, &first, "bcc", message->bcc, message->bcc_count) != 0)
        return -1;
    if (write_string_array_field(body, &first, "reply_to", message->reply_to,
                                 message->reply_to_count) != 0)
        return -1;
    if (write_string_field(body, &first, "subject", message->subject) != 0)
        return -1;
    if (write_string_field(body, &first, "html", message->html) != 0)
        return -1;
    if (write_string_field(body, &first, "text", message->text) != 0)
        return -1;

    if (message->header_count > 0) {
        if (write_field(body, &first, "headers") != 0)
            return -1;
        if (json_pair_object(body, message->headers, message->header_count) != 0)
            return -1;
    }
    if (message->tag_count > 0) {
        if (write_field(body, &first, "tags") != 0)
            return -1;
        if (json_tag_array(body, message->tags, message->tag_count) != 0)
            return -1;
    }
    if (message->attachment_count > 0) {
        if (write_field(body, &first, "attachments") != 0)
            return -1;
        if (write_attachments(body, message->attachments, message->attachment_count) != 0)
            return -1;
    }
    if (write_string_field(body, &first, "scheduled_at", message->scheduled_at) != 0)
        return -1;

    return json_object_end(body);
}

static int write_send_body(const resend_send *message, resend_buf *body) {
    resend_buf_init(body);
    if (write_send_object(message, body) != 0) {
        resend_buf_free(body);
        return -1;
    }
    return 0;
}

static int write_batch_body(const resend_batch *batch, resend_buf *body) {
    resend_buf_init(body);
    if (json_array_begin(body) != 0)
        goto fail;
    for (size_t i = 0; i < batch->count; i++) {
        if (i && json_comma(body) != 0)
            goto fail;
        if (write_send_object(&batch->messages[i], body) != 0)
            goto fail;
    }
    if (json_array_end(body) != 0)
        goto fail;
    return 0;
fail:
    resend_buf_free(body);
    return -1;
}

int resend_emails_send(resend *client, const resend_send *message, resend_sent *out) {
    if (!client || !message || !out)
        return -1;
    resend_error_clear(client);
    out->id = NULL;

    if (reject_bad_attachments(client, message->attachments, message->attachment_count) != 0)
        return -1;
    if (message->idempotency_key && strlen(message->idempotency_key) > 256) {
        resend_error_set(client, 0, NULL, "idempotency key longer than 256");
        return -1;
    }

    resend_buf body;
    if (write_send_body(message, &body) != 0) {
        resend_error_set(client, 0, NULL, "out of memory");
        return -1;
    }

    resend_http_response resp;
    if (resend_http_json(client, "POST", "/emails", body.data, message->idempotency_key,
                         &resp) != 0) {
        resend_buf_free(&body);
        return -1;
    }
    resend_buf_free(&body);

    if (resp.status < 200 || resp.status >= 300) {
        size_t len = resp.body.data ? resp.body.len : 0;
        resend_error_from_body(client, (int)resp.status, resp.body.data, len);
        resend_http_response_free(&resp);
        return -1;
    }

    char *id = NULL;
    size_t len = resp.body.data ? resp.body.len : 0;
    if (json_get_string(resp.body.data ? resp.body.data : "", len, "id", &id) != 0 || !id) {
        free(id);
        resend_error_set(client, (int)resp.status, NULL, "missing id");
        resend_http_response_free(&resp);
        return -1;
    }
    resend_http_response_free(&resp);
    out->id = id;
    return 0;
}

void resend_sent_free(resend_sent *sent) {
    if (!sent)
        return;
    free(sent->id);
    sent->id = NULL;
}

void resend_sent_list_free(resend_sent_list *list) {
    if (!list)
        return;
    if (list->items) {
        for (size_t i = 0; i < list->count; i++)
            resend_sent_free(&list->items[i]);
        free(list->items);
    }
    memset(list, 0, sizeof *list);
}

static int parse_sent_list(const char *json, size_t len, resend_sent_list *out) {
    memset(out, 0, sizeof *out);
    const char *val = NULL;
    const char *val_end = NULL;
    if (json_member(json, len, "data", &val, &val_end) != 0)
        return -1;
    val = json_skip_ws(val, val_end);
    if (val >= val_end || *val != '[')
        return -1;

    const char *p = val + 1;
    resend_sent *items = NULL;
    size_t n = 0;
    size_t cap = 0;
    for (;;) {
        p = json_skip_ws(p, val_end);
        if (p < val_end && *p == ']')
            break;
        if (p >= val_end || *p != '{')
            goto fail;
        const char *obj = p;
        if (json_skip_value(p, val_end, &p) != 0)
            goto fail;
        char *id = NULL;
        if (json_opt_string(obj, (size_t)(p - obj), "id", &id) != 0 || !id) {
            free(id);
            goto fail;
        }
        if (n == cap) {
            size_t ncap = cap ? cap * 2 : 4;
            resend_sent *ni = realloc(items, ncap * sizeof *ni);
            if (!ni) {
                free(id);
                goto fail;
            }
            items = ni;
            cap = ncap;
        }
        items[n].id = id;
        n++;
        int step = json_array_step(&p, val_end);
        if (step < 0)
            goto fail;
        if (step == 0)
            break;
    }
    out->items = items;
    out->count = n;
    return 0;
fail:
    {
        resend_sent_list tmp = {.items = items, .count = n};
        resend_sent_list_free(&tmp);
    }
    return -1;
}

int resend_emails_send_batch(resend *client, const resend_batch *batch, resend_sent_list *out) {
    if (!client || !batch || !out)
        return -1;
    resend_error_clear(client);
    memset(out, 0, sizeof *out);

    if (batch->count > 100) {
        resend_error_set(client, 0, NULL, "batch count above 100");
        return -1;
    }
    if (batch->count > 0 && !batch->messages) {
        resend_error_set(client, 0, NULL, "missing batch messages");
        return -1;
    }
    if (batch->idempotency_key && strlen(batch->idempotency_key) > 256) {
        resend_error_set(client, 0, NULL, "idempotency key longer than 256");
        return -1;
    }
    for (size_t i = 0; i < batch->count; i++) {
        if (batch->messages[i].attachment_count > 0) {
            resend_error_set(client, 0, NULL, "batch does not accept attachments");
            return -1;
        }
    }

    resend_buf body;
    if (write_batch_body(batch, &body) != 0) {
        resend_error_set(client, 0, NULL, "out of memory");
        return -1;
    }

    resend_http_response resp;
    if (resend_http_json(client, "POST", "/emails/batch", body.data, batch->idempotency_key,
                         &resp) != 0) {
        resend_buf_free(&body);
        return -1;
    }
    resend_buf_free(&body);

    if (resp.status < 200 || resp.status >= 300) {
        size_t len = resp.body.data ? resp.body.len : 0;
        resend_error_from_body(client, (int)resp.status, resp.body.data, len);
        resend_http_response_free(&resp);
        return -1;
    }

    size_t len = resp.body.data ? resp.body.len : 0;
    if (parse_sent_list(resp.body.data ? resp.body.data : "", len, out) != 0) {
        resend_sent_list_free(out);
        resend_error_set(client, (int)resp.status, NULL, "invalid batch response");
        resend_http_response_free(&resp);
        return -1;
    }
    resend_http_response_free(&resp);
    return 0;
}

static void free_owned_strings(char **items, size_t count) {
    if (!items)
        return;
    for (size_t i = 0; i < count; i++)
        free(items[i]);
    free(items);
}

void resend_email_free(resend_email *email) {
    if (!email)
        return;
    free(email->id);
    free(email->message_id);
    free(email->from);
    free_owned_strings(email->to, email->to_count);
    free_owned_strings(email->cc, email->cc_count);
    free_owned_strings(email->bcc, email->bcc_count);
    free_owned_strings(email->reply_to, email->reply_to_count);
    free(email->subject);
    free(email->html);
    free(email->text);
    free(email->created_at);
    free(email->last_event);
    memset(email, 0, sizeof *email);
}

static int parse_email(const char *json, size_t len, resend_email *out) {
    memset(out, 0, sizeof *out);
    if (json_opt_string(json, len, "id", &out->id) != 0 ||
        json_opt_string(json, len, "message_id", &out->message_id) != 0 ||
        json_opt_string(json, len, "from", &out->from) != 0 ||
        json_opt_string(json, len, "subject", &out->subject) != 0 ||
        json_opt_string(json, len, "html", &out->html) != 0 ||
        json_opt_string(json, len, "text", &out->text) != 0 ||
        json_opt_string(json, len, "created_at", &out->created_at) != 0 ||
        json_opt_string(json, len, "last_event", &out->last_event) != 0 ||
        json_opt_string_array(json, len, "to", &out->to, &out->to_count) != 0 ||
        json_opt_string_array(json, len, "cc", &out->cc, &out->cc_count) != 0 ||
        json_opt_string_array(json, len, "bcc", &out->bcc, &out->bcc_count) != 0 ||
        json_opt_string_array(json, len, "reply_to", &out->reply_to, &out->reply_to_count) != 0 ||
        !out->id) {
        resend_email_free(out);
        return -1;
    }
    return 0;
}

int resend_emails_get(resend *client, const char *email_id, resend_email *out) {
    if (!client || !email_id || !email_id[0] || !out)
        return -1;
    resend_error_clear(client);
    memset(out, 0, sizeof *out);

    char *path = NULL;
    if (resend_path_with_id(client, "/emails/", email_id, NULL, &path) != 0)
        return -1;

    resend_http_response resp;
    if (resend_http_json(client, "GET", path, NULL, NULL, &resp) != 0) {
        free(path);
        return -1;
    }
    free(path);

    if (resp.status < 200 || resp.status >= 300) {
        size_t len = resp.body.data ? resp.body.len : 0;
        resend_error_from_body(client, (int)resp.status, resp.body.data, len);
        resend_http_response_free(&resp);
        return -1;
    }

    size_t len = resp.body.data ? resp.body.len : 0;
    if (parse_email(resp.body.data ? resp.body.data : "", len, out) != 0) {
        resend_email_free(out);
        resend_error_set(client, (int)resp.status, NULL, "invalid email");
        resend_http_response_free(&resp);
        return -1;
    }
    resend_http_response_free(&resp);
    return 0;
}

void resend_email_list_free(resend_email_list *list) {
    if (!list)
        return;
    if (list->items) {
        for (size_t i = 0; i < list->count; i++)
            resend_email_free(&list->items[i]);
        free(list->items);
    }
    memset(list, 0, sizeof *list);
}

static int parse_email_list(const char *json, size_t len, resend_email_list *out) {
    memset(out, 0, sizeof *out);
    if (json_opt_bool(json, len, "has_more", &out->has_more) != 0)
        return -1;

    const char *val = NULL;
    const char *val_end = NULL;
    /* A well-formed object may omit data. Null data is an empty page. */
    int found = json_member(json, len, "data", &val, &val_end);
    if (found < 0)
        return -1;
    if (found > 0)
        return 0;
    val = json_skip_ws(val, val_end);
    if (json_is_null(val, val_end))
        return 0;
    if (val >= val_end || *val != '[')
        return -1;

    const char *p = val + 1;
    resend_email *items = NULL;
    size_t n = 0;
    size_t cap = 0;
    for (;;) {
        p = json_skip_ws(p, val_end);
        if (p < val_end && *p == ']')
            break;
        if (p >= val_end || *p != '{')
            goto fail;
        const char *obj = p;
        if (json_skip_value(p, val_end, &p) != 0)
            goto fail;
        if (n == cap) {
            size_t ncap = cap ? cap * 2 : 4;
            resend_email *ni = realloc(items, ncap * sizeof *ni);
            if (!ni)
                goto fail;
            items = ni;
            cap = ncap;
        }
        if (parse_email(obj, (size_t)(p - obj), &items[n]) != 0)
            goto fail;
        n++;
        int step = json_array_step(&p, val_end);
        if (step < 0)
            goto fail;
        if (step == 0)
            break;
    }
    out->items = items;
    out->count = n;
    return 0;
fail:
    {
        resend_email_list tmp = {.items = items, .count = n};
        resend_email_list_free(&tmp);
    }
    return -1;
}

int resend_emails_cancel(resend *client, const char *email_id, resend_email *out) {
    if (!client || !email_id || !email_id[0] || !out)
        return -1;
    resend_error_clear(client);
    memset(out, 0, sizeof *out);

    char *path = NULL;
    if (resend_path_with_id(client, "/emails/", email_id, "/cancel", &path) != 0)
        return -1;

    resend_http_response resp;
    if (resend_http_json(client, "POST", path, NULL, NULL, &resp) != 0) {
        free(path);
        return -1;
    }
    free(path);

    if (resp.status < 200 || resp.status >= 300) {
        size_t len = resp.body.data ? resp.body.len : 0;
        resend_error_from_body(client, (int)resp.status, resp.body.data, len);
        resend_http_response_free(&resp);
        return -1;
    }

    size_t len = resp.body.data ? resp.body.len : 0;
    if (parse_email(resp.body.data ? resp.body.data : "", len, out) != 0) {
        resend_email_free(out);
        resend_error_set(client, (int)resp.status, NULL, "invalid email");
        resend_http_response_free(&resp);
        return -1;
    }
    resend_http_response_free(&resp);
    return 0;
}

int resend_emails_list(resend *client, const resend_page *page, resend_email_list *out) {
    if (!client || !out)
        return -1;
    resend_error_clear(client);
    memset(out, 0, sizeof *out);

    char *path = NULL;
    if (resend_path_with_page(client, "/emails", page, &path) != 0)
        return -1;

    resend_http_response resp;
    if (resend_http_json(client, "GET", path, NULL, NULL, &resp) != 0) {
        free(path);
        return -1;
    }
    free(path);

    if (resp.status < 200 || resp.status >= 300) {
        size_t len = resp.body.data ? resp.body.len : 0;
        resend_error_from_body(client, (int)resp.status, resp.body.data, len);
        resend_http_response_free(&resp);
        return -1;
    }

    size_t len = resp.body.data ? resp.body.len : 0;
    if (parse_email_list(resp.body.data ? resp.body.data : "", len, out) != 0) {
        resend_email_list_free(out);
        resend_error_set(client, (int)resp.status, NULL, "invalid email list");
        resend_http_response_free(&resp);
        return -1;
    }
    resend_http_response_free(&resp);
    return 0;
}
