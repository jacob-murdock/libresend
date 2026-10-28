#include "internal.h"

#include <stdlib.h>
#include <string.h>

void resend_suppression_free(resend_suppression *suppression) {
    if (!suppression)
        return;
    free(suppression->id);
    free(suppression->email);
    free(suppression->origin);
    free(suppression->source_id);
    free(suppression->created_at);
    memset(suppression, 0, sizeof *suppression);
}

void resend_suppression_list_free(resend_suppression_list *list) {
    if (!list)
        return;
    if (list->items) {
        for (size_t i = 0; i < list->count; i++)
            resend_suppression_free(&list->items[i]);
        free(list->items);
    }
    memset(list, 0, sizeof *list);
}

static int parse_suppression(const char *json, size_t len, resend_suppression *out) {
    memset(out, 0, sizeof *out);
    if (json_opt_string(json, len, "id", &out->id) != 0 ||
        json_opt_string(json, len, "email", &out->email) != 0 ||
        json_opt_string(json, len, "origin", &out->origin) != 0 ||
        json_opt_string(json, len, "source_id", &out->source_id) != 0 ||
        json_opt_string(json, len, "created_at", &out->created_at) != 0 ||
        json_opt_bool(json, len, "deleted", &out->deleted) != 0 || !out->id) {
        resend_suppression_free(out);
        return -1;
    }
    return 0;
}

static int parse_suppression_list(const char *json, size_t len, resend_suppression_list *out) {
    memset(out, 0, sizeof *out);
    if (json_opt_bool(json, len, "has_more", &out->has_more) != 0)
        return -1;

    const char *val = NULL;
    const char *val_end = NULL;
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
    resend_suppression *items = NULL;
    size_t n = 0;
    size_t cap = 0;
    for (;;) {
        p = json_skip_ws(p, val_end);
        if (p < val_end && *p == ']')
            break;
        if (p >= val_end || *p != '{')
            goto fail;
        const char *obj = p;
        if (json_skip_value(p, val_end, &p) != 0 || p == obj)
            goto fail;
        if (n == cap) {
            size_t ncap = cap ? cap * 2 : 4;
            resend_suppression *ni = realloc(items, ncap * sizeof *ni);
            if (!ni)
                goto fail;
            items = ni;
            cap = ncap;
        }
        if (parse_suppression(obj, (size_t)(p - obj), &items[n]) != 0)
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
        resend_suppression_list tmp = {.items = items, .count = n};
        resend_suppression_list_free(&tmp);
    }
    return -1;
}

static int write_string_object(resend_buf *body, const char *key, const char *value) {
    resend_buf_init(body);
    if (json_object_begin(body) != 0 || json_key(body, key) != 0 || json_string(body, value) != 0 ||
        json_object_end(body) != 0) {
        resend_buf_free(body);
        return -1;
    }
    return 0;
}

static int write_string_array_object(resend_buf *body, const char *key, const char *const *items,
                                     size_t count) {
    resend_buf_init(body);
    if (json_object_begin(body) != 0 || json_key(body, key) != 0 ||
        json_string_array(body, items, count) != 0 || json_object_end(body) != 0) {
        resend_buf_free(body);
        return -1;
    }
    return 0;
}

int resend_suppressions_add(resend *client, const char *email, resend_suppression *out) {
    if (!client || !out)
        return -1;
    resend_error_clear(client);
    memset(out, 0, sizeof *out);
    if (!email || !email[0]) {
        resend_error_set(client, 0, NULL, "missing email");
        return -1;
    }

    resend_buf body;
    if (write_string_object(&body, "email", email) != 0) {
        resend_error_set(client, 0, NULL, "out of memory");
        return -1;
    }

    resend_http_response resp;
    if (resend_http_json(client, "POST", "/suppressions", body.data, NULL, &resp) != 0) {
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
    if (parse_suppression(resp.body.data ? resp.body.data : "", len, out) != 0) {
        resend_suppression_free(out);
        resend_error_set(client, (int)resp.status, NULL, "invalid suppression");
        resend_http_response_free(&resp);
        return -1;
    }
    resend_http_response_free(&resp);
    return 0;
}

int resend_suppressions_list(resend *client, const resend_page *page, const char *origin,
                             resend_suppression_list *out) {
    if (!client || !out)
        return -1;
    resend_error_clear(client);
    memset(out, 0, sizeof *out);

    char *path = NULL;
    if (resend_path_with_page(client, "/suppressions", page, &path) != 0)
        return -1;
    if (resend_path_append_query(client, &path, "origin", origin) != 0) {
        free(path);
        return -1;
    }

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
    if (parse_suppression_list(resp.body.data ? resp.body.data : "", len, out) != 0) {
        resend_suppression_list_free(out);
        resend_error_set(client, (int)resp.status, NULL, "invalid suppression list");
        resend_http_response_free(&resp);
        return -1;
    }
    resend_http_response_free(&resp);
    return 0;
}

static int suppression_by_id(resend *client, const char *method, const char *suppression,
                             resend_suppression *out) {
    if (!client || !suppression || !suppression[0] || !out)
        return -1;
    resend_error_clear(client);
    memset(out, 0, sizeof *out);

    char *path = NULL;
    if (resend_path_with_id(client, "/suppressions/", suppression, NULL, &path) != 0)
        return -1;

    resend_http_response resp;
    if (resend_http_json(client, method, path, NULL, NULL, &resp) != 0) {
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
    if (parse_suppression(resp.body.data ? resp.body.data : "", len, out) != 0) {
        resend_suppression_free(out);
        resend_error_set(client, (int)resp.status, NULL, "invalid suppression");
        resend_http_response_free(&resp);
        return -1;
    }
    resend_http_response_free(&resp);
    return 0;
}

int resend_suppressions_get(resend *client, const char *suppression, resend_suppression *out) {
    return suppression_by_id(client, "GET", suppression, out);
}

int resend_suppressions_remove(resend *client, const char *suppression, resend_suppression *out) {
    return suppression_by_id(client, "DELETE", suppression, out);
}

int resend_suppressions_add_batch(resend *client, const char *const *emails, size_t count,
                                  resend_suppression_list *out) {
    if (!client || !out)
        return -1;
    resend_error_clear(client);
    memset(out, 0, sizeof *out);
    if (count == 0) {
        resend_error_set(client, 0, NULL, "batch count below 1");
        return -1;
    }
    if (count > 100) {
        resend_error_set(client, 0, NULL, "batch count above 100");
        return -1;
    }
    if (!emails) {
        resend_error_set(client, 0, NULL, "missing batch emails");
        return -1;
    }

    resend_buf body;
    if (write_string_array_object(&body, "emails", emails, count) != 0) {
        resend_error_set(client, 0, NULL, "out of memory");
        return -1;
    }

    resend_http_response resp;
    if (resend_http_json(client, "POST", "/suppressions/batch/add", body.data, NULL, &resp) != 0) {
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
    if (parse_suppression_list(resp.body.data ? resp.body.data : "", len, out) != 0) {
        resend_suppression_list_free(out);
        resend_error_set(client, (int)resp.status, NULL, "invalid suppression list");
        resend_http_response_free(&resp);
        return -1;
    }
    resend_http_response_free(&resp);
    return 0;
}

int resend_suppressions_remove_batch(resend *client, const resend_suppression_batch_remove *req,
                                     resend_suppression_list *out) {
    if (!client || !req || !out)
        return -1;
    resend_error_clear(client);
    memset(out, 0, sizeof *out);

    int have_emails = req->email_count > 0;
    int have_ids = req->id_count > 0;
    if (have_emails && have_ids) {
        resend_error_set(client, 0, NULL, "emails and ids cannot be combined");
        return -1;
    }
    if (!have_emails && !have_ids) {
        resend_error_set(client, 0, NULL, "missing batch emails or ids");
        return -1;
    }

    const char *key;
    const char *const *items;
    size_t count;
    const char *missing;
    if (have_emails) {
        key = "emails";
        items = req->emails;
        count = req->email_count;
        missing = "missing batch emails";
    } else {
        key = "ids";
        items = req->ids;
        count = req->id_count;
        missing = "missing batch ids";
    }
    if (count > 100) {
        resend_error_set(client, 0, NULL, "batch count above 100");
        return -1;
    }
    if (!items) {
        resend_error_set(client, 0, NULL, missing);
        return -1;
    }

    resend_buf body;
    if (write_string_array_object(&body, key, items, count) != 0) {
        resend_error_set(client, 0, NULL, "out of memory");
        return -1;
    }

    resend_http_response resp;
    if (resend_http_json(client, "POST", "/suppressions/batch/remove", body.data, NULL, &resp) !=
        0) {
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
    if (parse_suppression_list(resp.body.data ? resp.body.data : "", len, out) != 0) {
        resend_suppression_list_free(out);
        resend_error_set(client, (int)resp.status, NULL, "invalid suppression list");
        resend_http_response_free(&resp);
        return -1;
    }
    resend_http_response_free(&resp);
    return 0;
}
