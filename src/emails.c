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

static int write_send_body(const resend_send *message, resend_buf *body) {
    resend_buf_init(body);
    if (json_object_begin(body) != 0)
        goto fail;

    int first = 1;
    if (write_string_field(body, &first, "from", message->from) != 0)
        goto fail;
    if (write_string_array_field(body, &first, "to", message->to, message->to_count) != 0)
        goto fail;
    if (write_string_array_field(body, &first, "cc", message->cc, message->cc_count) != 0)
        goto fail;
    if (write_string_array_field(body, &first, "bcc", message->bcc, message->bcc_count) != 0)
        goto fail;
    if (write_string_array_field(body, &first, "reply_to", message->reply_to,
                                 message->reply_to_count) != 0)
        goto fail;
    if (write_string_field(body, &first, "subject", message->subject) != 0)
        goto fail;
    if (write_string_field(body, &first, "html", message->html) != 0)
        goto fail;
    if (write_string_field(body, &first, "text", message->text) != 0)
        goto fail;

    if (message->header_count > 0) {
        if (write_field(body, &first, "headers") != 0)
            goto fail;
        if (json_pair_object(body, message->headers, message->header_count) != 0)
            goto fail;
    }
    if (message->tag_count > 0) {
        if (write_field(body, &first, "tags") != 0)
            goto fail;
        if (json_tag_array(body, message->tags, message->tag_count) != 0)
            goto fail;
    }
    if (message->attachment_count > 0) {
        if (write_field(body, &first, "attachments") != 0)
            goto fail;
        if (write_attachments(body, message->attachments, message->attachment_count) != 0)
            goto fail;
    }
    if (write_string_field(body, &first, "scheduled_at", message->scheduled_at) != 0)
        goto fail;

    if (json_object_end(body) != 0)
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

    for (size_t i = 0; i < message->attachment_count; i++) {
        if (!attachment_mode_ok(&message->attachments[i])) {
            resend_error_set(client, 0, NULL, "attachment needs content or path, not both");
            return -1;
        }
    }
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
