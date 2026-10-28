#include "internal.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>

static char *dup_span(const char *start, const char *end) {
    size_t n = (size_t)(end - start);
    char *s = malloc(n + 1);
    if (!s)
        return NULL;
    memcpy(s, start, n);
    s[n] = '\0';
    return s;
}

void resend_log_free(resend_log *log) {
    if (!log)
        return;
    free(log->id);
    free(log->created_at);
    free(log->endpoint);
    free(log->method);
    free(log->user_agent);
    free(log->request_body);
    free(log->response_body);
    memset(log, 0, sizeof *log);
}

void resend_log_list_free(resend_log_list *list) {
    if (!list)
        return;
    if (list->items) {
        for (size_t i = 0; i < list->count; i++)
            resend_log_free(&list->items[i]);
        free(list->items);
    }
    memset(list, 0, sizeof *list);
}

/* Missing or null JSON value stays NULL. Any other value is copied as received. */
static int opt_raw_json(const char *json, size_t len, const char *key, char **out) {
    *out = NULL;
    const char *val = NULL;
    const char *val_end = NULL;
    int st = resend_json_member(json, len, key, &val, &val_end);
    if (st < 0)
        return -1;
    if (st > 0)
        return 0;
    val = resend_json_skip_ws(val, val_end);
    if (resend_json_is_null(val, val_end))
        return 0;
    *out = dup_span(val, val_end);
    return *out ? 0 : -1;
}

static int parse_log(const char *json, size_t len, resend_log *out) {
    memset(out, 0, sizeof *out);
    int64_t status = 0;
    if (resend_json_opt_string(json, len, "id", &out->id) != 0 ||
        resend_json_opt_string(json, len, "created_at", &out->created_at) != 0 ||
        resend_json_opt_string(json, len, "endpoint", &out->endpoint) != 0 ||
        resend_json_opt_string(json, len, "method", &out->method) != 0 ||
        resend_json_opt_string(json, len, "user_agent", &out->user_agent) != 0 ||
        resend_json_opt_i64(json, len, "response_status", &status) != 0 || status < INT_MIN ||
        status > INT_MAX || opt_raw_json(json, len, "request_body", &out->request_body) != 0 ||
        opt_raw_json(json, len, "response_body", &out->response_body) != 0 || !out->id) {
        resend_log_free(out);
        return -1;
    }
    out->response_status = (int)status;
    return 0;
}

static int parse_log_list(const char *json, size_t len, resend_log_list *out) {
    memset(out, 0, sizeof *out);
    if (resend_json_opt_bool(json, len, "has_more", &out->has_more) != 0)
        return -1;

    const char *val = NULL;
    const char *val_end = NULL;
    int found = resend_json_member(json, len, "data", &val, &val_end);
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
    resend_log *items = NULL;
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
            size_t ncap = cap ? cap * 2 : 4;
            resend_log *ni = realloc(items, ncap * sizeof *ni);
            if (!ni)
                goto fail;
            items = ni;
            cap = ncap;
        }
        if (parse_log(obj, (size_t)(p - obj), &items[n]) != 0)
            goto fail;
        n++;
        int step = resend_json_array_step(&p, val_end);
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
        resend_log_list tmp = {.items = items, .count = n};
        resend_log_list_free(&tmp);
    }
    return -1;
}

int resend_logs_list(resend *client, const resend_page *page, resend_log_list *out) {
    if (!client || !out)
        return -1;
    resend_error_clear(client);
    memset(out, 0, sizeof *out);

    char *path = NULL;
    if (resend_path_with_page(client, "/logs", page, &path) != 0)
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
    if (parse_log_list(resp.body.data ? resp.body.data : "", len, out) != 0) {
        resend_log_list_free(out);
        resend_error_set(client, (int)resp.status, NULL, "invalid log list");
        resend_http_response_free(&resp);
        return -1;
    }
    resend_http_response_free(&resp);
    return 0;
}

int resend_logs_get(resend *client, const char *log_id, resend_log *out) {
    if (!client || !log_id || !log_id[0] || !out)
        return -1;
    resend_error_clear(client);
    memset(out, 0, sizeof *out);

    char *path = NULL;
    if (resend_path_with_id(client, "/logs/", log_id, NULL, &path) != 0)
        return -1;

    resend_http_response resp;
    if (resend_http_json_limit(client, "GET", path, NULL, NULL, NULL, LIBRESEND_LARGE_BODY_MAX,
                               &resp) != 0) {
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
    if (parse_log(resp.body.data ? resp.body.data : "", len, out) != 0) {
        resend_log_free(out);
        resend_error_set(client, (int)resp.status, NULL, "invalid log");
        resend_http_response_free(&resp);
        return -1;
    }
    resend_http_response_free(&resp);
    return 0;
}
