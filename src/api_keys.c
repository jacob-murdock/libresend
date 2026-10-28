#include "internal.h"

#include <openssl/crypto.h>
#include <stdlib.h>
#include <string.h>

void resend_api_key_free(resend_api_key *key) {
    if (!key)
        return;
    free(key->id);
    free(key->name);
    free(key->created_at);
    free(key->last_used_at);
    if (key->token) {
        OPENSSL_cleanse(key->token, strlen(key->token));
        free(key->token);
    }
    memset(key, 0, sizeof *key);
}

void resend_api_key_list_free(resend_api_key_list *list) {
    if (!list)
        return;
    if (list->items) {
        for (size_t i = 0; i < list->count; i++)
            resend_api_key_free(&list->items[i]);
        free(list->items);
    }
    memset(list, 0, sizeof *list);
}

/* Create fills id and token. List fills name, created_at, and last_used_at. */
static int parse_api_key(const char *json, size_t len, resend_api_key *out, int from_create) {
    memset(out, 0, sizeof *out);
    if (from_create) {
        if (resend_json_opt_string(json, len, "id", &out->id) != 0 ||
            resend_json_opt_string(json, len, "token", &out->token) != 0 || !out->id || !out->token) {
            resend_api_key_free(out);
            return -1;
        }
        return 0;
    }
    if (resend_json_opt_string(json, len, "id", &out->id) != 0 ||
        resend_json_opt_string(json, len, "name", &out->name) != 0 ||
        resend_json_opt_string(json, len, "created_at", &out->created_at) != 0 ||
        resend_json_opt_string(json, len, "last_used_at", &out->last_used_at) != 0 || !out->id) {
        resend_api_key_free(out);
        return -1;
    }
    return 0;
}

static int parse_api_key_list(const char *json, size_t len, resend_api_key_list *out) {
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
    resend_api_key *items = NULL;
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
            resend_api_key *ni = realloc(items, ncap * sizeof *ni);
            if (!ni)
                goto fail;
            items = ni;
            cap = ncap;
        }
        if (parse_api_key(obj, (size_t)(p - obj), &items[n], 0) != 0)
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
        resend_api_key_list tmp = {.items = items, .count = n};
        resend_api_key_list_free(&tmp);
    }
    return -1;
}

static int write_field(resend_buf *body, int *first, const char *key) {
    if (!*first && resend_json_comma(body) != 0)
        return -1;
    *first = 0;
    return resend_json_key(body, key);
}

static int write_string_field(resend_buf *body, int *first, const char *key, const char *value) {
    if (!value || !value[0])
        return 0;
    if (write_field(body, first, key) != 0)
        return -1;
    return resend_json_string(body, value);
}

static int permission_ok(const char *permission) {
    return strcmp(permission, "full_access") == 0 || strcmp(permission, "sending_access") == 0;
}

static int write_key_body(const resend_api_key_create *key, resend_buf *body) {
    resend_buf_init(body);
    int first = 1;
    if (resend_json_object_begin(body) != 0)
        goto fail;
    if (write_string_field(body, &first, "name", key->name) != 0 ||
        write_string_field(body, &first, "permission", key->permission) != 0 ||
        write_string_field(body, &first, "domain_id", key->domain_id) != 0 ||
        resend_json_object_end(body) != 0)
        goto fail;
    return 0;
fail:
    resend_buf_free(body);
    return -1;
}

int resend_api_keys_create(resend *client, const resend_api_key_create *key, resend_api_key *out) {
    if (!client)
        return -1;
    resend_error_clear(client);
    if (!key) {
        resend_error_set(client, 0, "missing_required_field", "missing request");
        return -1;
    }
    if (!out) {
        resend_error_set(client, 0, "missing_required_field", "missing result");
        return -1;
    }
    memset(out, 0, sizeof *out);
    if (!key->name || !key->name[0]) {
        resend_error_set(client, 0, "missing_required_field", "missing api key name");
        return -1;
    }
    if (key->permission && !permission_ok(key->permission)) {
        resend_error_set(client, 0, "validation_error", "permission must be full_access or sending_access");
        return -1;
    }
    if (key->domain_id && key->domain_id[0] &&
        (!key->permission || strcmp(key->permission, "sending_access") != 0)) {
        resend_error_set(client, 0, "validation_error", "domain_id requires sending_access");
        return -1;
    }

    resend_buf body;
    if (write_key_body(key, &body) != 0) {
        resend_error_set(client, 0, NULL, "out of memory");
        return -1;
    }

    resend_http_response resp;
    if (resend_http_json(client, "POST", "/api-keys", body.data, NULL, &resp) != 0) {
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
    int rc = parse_api_key(resp.body.data ? resp.body.data : "", len, out, 1);
    int status = (int)resp.status;
    if (resp.body.data)
        OPENSSL_cleanse(resp.body.data, resp.body.len); /* the body holds the new token */
    resend_http_response_free(&resp);
    if (rc != 0) {
        resend_api_key_free(out);
        resend_error_set(client, status, NULL, "invalid api key");
        return -1;
    }
    return 0;
}

int resend_api_keys_list(resend *client, const resend_page *page, resend_api_key_list *out) {
    if (!client)
        return -1;
    resend_error_clear(client);
    if (!out) {
        resend_error_set(client, 0, "missing_required_field", "missing result");
        return -1;
    }
    memset(out, 0, sizeof *out);

    char *path = NULL;
    if (resend_path_with_page(client, "/api-keys", page, &path) != 0)
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
    if (parse_api_key_list(resp.body.data ? resp.body.data : "", len, out) != 0) {
        resend_api_key_list_free(out);
        resend_error_set(client, (int)resp.status, NULL, "invalid api key list");
        resend_http_response_free(&resp);
        return -1;
    }
    resend_http_response_free(&resp);
    return 0;
}

/* PATCH /api-keys/{id} with {"name": ...}. The response is only the id. */
int resend_api_keys_update(resend *client, const char *api_key_id, const char *name) {
    if (!client)
        return -1;
    resend_error_clear(client);
    if (!api_key_id || !api_key_id[0]) {
        resend_error_set(client, 0, "missing_required_field", "missing id");
        return -1;
    }
    if (!name || !name[0]) {
        resend_error_set(client, 0, "missing_required_field", "missing api key name");
        return -1;
    }

    resend_buf body;
    resend_buf_init(&body);
    if (resend_json_object_begin(&body) != 0 || resend_json_key(&body, "name") != 0 ||
        resend_json_string(&body, name) != 0 || resend_json_object_end(&body) != 0) {
        resend_buf_free(&body);
        resend_error_set(client, 0, NULL, "out of memory");
        return -1;
    }

    char *path = NULL;
    if (resend_path_with_id(client, "/api-keys/", api_key_id, NULL, &path) != 0) {
        resend_buf_free(&body);
        return -1;
    }

    resend_http_response resp;
    int rc = resend_http_json(client, "PATCH", path, body.data, NULL, &resp);
    free(path);
    resend_buf_free(&body);
    if (rc != 0)
        return -1;

    if (resp.status < 200 || resp.status >= 300) {
        size_t len = resp.body.data ? resp.body.len : 0;
        resend_error_from_body(client, (int)resp.status, resp.body.data, len);
        resend_http_response_free(&resp);
        return -1;
    }
    resend_http_response_free(&resp);
    return 0;
}

int resend_api_keys_remove(resend *client, const char *api_key_id) {
    if (!client)
        return -1;
    resend_error_clear(client);
    if (!api_key_id || !api_key_id[0]) {
        resend_error_set(client, 0, "missing_required_field", "missing id");
        return -1;
    }

    char *path = NULL;
    if (resend_path_with_id(client, "/api-keys/", api_key_id, NULL, &path) != 0)
        return -1;

    resend_http_response resp;
    if (resend_http_json(client, "DELETE", path, NULL, NULL, &resp) != 0) {
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
    resend_http_response_free(&resp);
    return 0;
}
