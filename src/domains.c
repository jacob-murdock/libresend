#include "internal.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>

static void free_record(resend_dns_record *rec) {
    if (!rec)
        return;
    free(rec->record);
    free(rec->name);
    free(rec->type);
    free(rec->value);
    free(rec->status);
    free(rec->ttl);
    memset(rec, 0, sizeof *rec);
}

void resend_domain_free(resend_domain *domain) {
    if (!domain)
        return;
    free(domain->id);
    free(domain->name);
    free(domain->status);
    free(domain->region);
    free(domain->sending);
    free(domain->receiving);
    free(domain->tracking_subdomain);
    free(domain->created_at);
    if (domain->records) {
        for (size_t i = 0; i < domain->record_count; i++)
            free_record(&domain->records[i]);
        free(domain->records);
    }
    memset(domain, 0, sizeof *domain);
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

/* Missing or null priority is -1. 0 is a real MX priority. */
static int record_priority(const char *json, size_t len, int *out) {
    const char *val = NULL;
    const char *val_end = NULL;
    int st = resend_json_member(json, len, "priority", &val, &val_end);
    if (st < 0)
        return -1;
    if (st > 0 || resend_json_is_null(resend_json_skip_ws(val, val_end), val_end)) {
        *out = -1;
        return 0;
    }
    int64_t n = 0;
    if (resend_json_opt_i64(json, len, "priority", &n) != 0 || n < INT_MIN || n > INT_MAX)
        return -1;
    *out = (int)n;
    return 0;
}

static int parse_record(const char *json, size_t len, resend_dns_record *out) {
    memset(out, 0, sizeof *out);
    out->priority = -1;
    if (resend_json_opt_string(json, len, "record", &out->record) != 0 ||
        resend_json_opt_string(json, len, "name", &out->name) != 0 ||
        resend_json_opt_string(json, len, "type", &out->type) != 0 ||
        resend_json_opt_string(json, len, "value", &out->value) != 0 ||
        resend_json_opt_string(json, len, "status", &out->status) != 0 ||
        resend_json_opt_string(json, len, "ttl", &out->ttl) != 0 ||
        record_priority(json, len, &out->priority) != 0) {
        free_record(out);
        return -1;
    }
    return 0;
}

static int parse_records(const char *json, size_t len, resend_dns_record **out, size_t *count) {
    *out = NULL;
    *count = 0;
    const char *val = NULL;
    const char *val_end = NULL;
    int st = resend_json_member(json, len, "records", &val, &val_end);
    if (st < 0)
        return -1;
    if (st > 0)
        return 0;
    val = resend_json_skip_ws(val, val_end);
    if (resend_json_is_null(val, val_end))
        return 0;
    if (val >= val_end || *val != '[')
        return -1;

    const char *p = val + 1;
    resend_dns_record *items = NULL;
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
            resend_dns_record *ni = realloc(items, ncap * sizeof *ni);
            if (!ni)
                goto fail;
            items = ni;
            cap = ncap;
        }
        if (parse_record(obj, (size_t)(p - obj), &items[n]) != 0)
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
    for (size_t i = 0; i < n; i++)
        free_record(&items[i]);
    free(items);
    return -1;
}

/* sending and receiving live under capabilities. Missing or null leaves both NULL. */
static int parse_capabilities(const char *json, size_t len, resend_domain *out) {
    const char *val = NULL;
    size_t n = 0;
    int st = object_member(json, len, "capabilities", &val, &n);
    if (st != 0)
        return st < 0 ? -1 : 0;
    if (resend_json_opt_string(val, n, "sending", &out->sending) != 0 ||
        resend_json_opt_string(val, n, "receiving", &out->receiving) != 0)
        return -1;
    return 0;
}

/*
 * with_detail is set for get and create. List leaves tracking_subdomain and
 * records NULL even when the payload carries them.
 */
static int parse_domain(const char *json, size_t len, resend_domain *out, int with_detail) {
    memset(out, 0, sizeof *out);
    if (resend_json_opt_string(json, len, "id", &out->id) != 0 ||
        resend_json_opt_string(json, len, "name", &out->name) != 0 ||
        resend_json_opt_string(json, len, "status", &out->status) != 0 ||
        resend_json_opt_string(json, len, "region", &out->region) != 0 ||
        resend_json_opt_string(json, len, "created_at", &out->created_at) != 0 ||
        resend_json_opt_bool(json, len, "open_tracking", &out->open_tracking) != 0 ||
        resend_json_opt_bool(json, len, "click_tracking", &out->click_tracking) != 0 ||
        parse_capabilities(json, len, out) != 0 ||
        (with_detail &&
         resend_json_opt_string(json, len, "tracking_subdomain", &out->tracking_subdomain) != 0) ||
        (with_detail && parse_records(json, len, &out->records, &out->record_count) != 0) ||
        !out->id) {
        resend_domain_free(out);
        return -1;
    }
    return 0;
}

int resend_domains_get(resend *client, const char *domain_id, resend_domain *out) {
    if (!client)
        return -1;
    resend_error_clear(client);
    if (!domain_id || !domain_id[0]) {
        resend_error_set(client, 0, "missing_required_field", "missing id");
        return -1;
    }
    if (!out) {
        resend_error_set(client, 0, "missing_required_field", "missing result");
        return -1;
    }
    memset(out, 0, sizeof *out);

    char *path = NULL;
    if (resend_path_with_id(client, "/domains/", domain_id, NULL, &path) != 0)
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
    if (parse_domain(resp.body.data ? resp.body.data : "", len, out, 1) != 0) {
        resend_domain_free(out);
        resend_error_set(client, (int)resp.status, NULL, "invalid domain");
        resend_http_response_free(&resp);
        return -1;
    }
    resend_http_response_free(&resp);
    return 0;
}

void resend_domain_list_free(resend_domain_list *list) {
    if (!list)
        return;
    if (list->items) {
        for (size_t i = 0; i < list->count; i++)
            resend_domain_free(&list->items[i]);
        free(list->items);
    }
    memset(list, 0, sizeof *list);
}

static int parse_domain_list(const char *json, size_t len, resend_domain_list *out) {
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
    resend_domain *items = NULL;
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
            resend_domain *ni = realloc(items, ncap * sizeof *ni);
            if (!ni)
                goto fail;
            items = ni;
            cap = ncap;
        }
        if (parse_domain(obj, (size_t)(p - obj), &items[n], 0) != 0)
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
        resend_domain_list tmp = {.items = items, .count = n};
        resend_domain_list_free(&tmp);
    }
    return -1;
}

int resend_domains_list(resend *client, const resend_page *page, resend_domain_list *out) {
    if (!client)
        return -1;
    resend_error_clear(client);
    if (!out) {
        resend_error_set(client, 0, "missing_required_field", "missing result");
        return -1;
    }
    memset(out, 0, sizeof *out);

    char *path = NULL;
    if (resend_path_with_page(client, "/domains", page, &path) != 0)
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
    if (parse_domain_list(resp.body.data ? resp.body.data : "", len, out) != 0) {
        resend_domain_list_free(out);
        resend_error_set(client, (int)resp.status, NULL, "invalid domain list");
        resend_http_response_free(&resp);
        return -1;
    }
    resend_http_response_free(&resp);
    return 0;
}

static int write_field(resend_buf *body, int *first, const char *key) {
    if (!*first && resend_json_comma(body) != 0)
        return -1;
    *first = 0;
    return resend_json_key(body, key);
}

static int write_string_field(resend_buf *body, int *first, const char *key, const char *value) {
    if (!value)
        return 0;
    if (write_field(body, first, key) != 0)
        return -1;
    return resend_json_string(body, value);
}

static int write_bool_field(resend_buf *body, int *first, const char *key, const int *value) {
    if (!value)
        return 0;
    if (write_field(body, first, key) != 0)
        return -1;
    return resend_buf_puts(body, *value ? "true" : "false");
}

/* NULL caps omits the key. Each NULL field is left out, so {0} sends {}. */
static int write_capabilities(resend_buf *body, int *first, const resend_capabilities *caps) {
    if (!caps)
        return 0;
    if (write_field(body, first, "capabilities") != 0 || resend_json_object_begin(body) != 0)
        return -1;
    int inner = 1;
    if (write_string_field(body, &inner, "sending", caps->sending) != 0)
        return -1;
    if (write_string_field(body, &inner, "receiving", caps->receiving) != 0)
        return -1;
    return resend_json_object_end(body);
}

static int write_create_body(const resend_domain_create *domain, resend_buf *body) {
    resend_buf_init(body);
    int first = 1;
    if (resend_json_object_begin(body) != 0)
        goto fail;
    if (write_string_field(body, &first, "name", domain->name) != 0 ||
        write_string_field(body, &first, "region", domain->region) != 0 ||
        write_string_field(body, &first, "custom_return_path", domain->custom_return_path) != 0 ||
        write_string_field(body, &first, "tls", domain->tls) != 0 ||
        write_string_field(body, &first, "tracking_subdomain", domain->tracking_subdomain) != 0 ||
        write_bool_field(body, &first, "open_tracking", domain->open_tracking) != 0 ||
        write_bool_field(body, &first, "click_tracking", domain->click_tracking) != 0 ||
        write_capabilities(body, &first, domain->capabilities) != 0 || resend_json_object_end(body) != 0)
        goto fail;
    return 0;
fail:
    resend_buf_free(body);
    return -1;
}

int resend_domains_create(resend *client, const resend_domain_create *domain, resend_domain *out) {
    if (!client)
        return -1;
    resend_error_clear(client);
    if (!domain) {
        resend_error_set(client, 0, "missing_required_field", "missing request");
        return -1;
    }
    if (!out) {
        resend_error_set(client, 0, "missing_required_field", "missing result");
        return -1;
    }
    memset(out, 0, sizeof *out);
    if (!domain->name || !domain->name[0]) {
        resend_error_set(client, 0, "missing_required_field", "missing domain name");
        return -1;
    }

    resend_buf body;
    if (write_create_body(domain, &body) != 0) {
        resend_error_set(client, 0, NULL, "out of memory");
        return -1;
    }

    resend_http_response resp;
    if (resend_http_json(client, "POST", "/domains", body.data, NULL, &resp) != 0) {
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
    if (parse_domain(resp.body.data ? resp.body.data : "", len, out, 1) != 0) {
        resend_domain_free(out);
        resend_error_set(client, (int)resp.status, NULL, "invalid domain");
        resend_http_response_free(&resp);
        return -1;
    }
    resend_http_response_free(&resp);
    return 0;
}

static int update_is_empty(const resend_domain_update *domain) {
    if (domain->tls || domain->tracking_subdomain || domain->open_tracking || domain->click_tracking)
        return 0;
    if (domain->capabilities)
        return 0;
    return 1;
}

static int write_update_body(const resend_domain_update *domain, resend_buf *body) {
    resend_buf_init(body);
    int first = 1;
    if (resend_json_object_begin(body) != 0)
        goto fail;
    if (write_string_field(body, &first, "tls", domain->tls) != 0 ||
        write_string_field(body, &first, "tracking_subdomain", domain->tracking_subdomain) != 0 ||
        write_bool_field(body, &first, "open_tracking", domain->open_tracking) != 0 ||
        write_bool_field(body, &first, "click_tracking", domain->click_tracking) != 0 ||
        write_capabilities(body, &first, domain->capabilities) != 0 || resend_json_object_end(body) != 0)
        goto fail;
    return 0;
fail:
    resend_buf_free(body);
    return -1;
}

static int domain_method(resend *client, const char *method, const char *domain_id, const char *suffix,
                         const char *body) {
    char *path = NULL;
    if (resend_path_with_id(client, "/domains/", domain_id, suffix, &path) != 0)
        return -1;

    resend_http_response resp;
    if (resend_http_json(client, method, path, body, NULL, &resp) != 0) {
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

int resend_domains_update(resend *client, const char *domain_id, const resend_domain_update *domain) {
    if (!client)
        return -1;
    resend_error_clear(client);
    if (!domain_id || !domain_id[0]) {
        resend_error_set(client, 0, "missing_required_field", "missing id");
        return -1;
    }
    if (!domain) {
        resend_error_set(client, 0, "missing_required_field", "missing request");
        return -1;
    }
    if (update_is_empty(domain)) {
        resend_error_set(client, 0, "validation_error", "domain update needs a field");
        return -1;
    }

    resend_buf body;
    if (write_update_body(domain, &body) != 0) {
        resend_error_set(client, 0, NULL, "out of memory");
        return -1;
    }
    int rc = domain_method(client, "PATCH", domain_id, NULL, body.data);
    resend_buf_free(&body);
    return rc;
}

int resend_domains_verify(resend *client, const char *domain_id) {
    if (!client)
        return -1;
    resend_error_clear(client);
    if (!domain_id || !domain_id[0]) {
        resend_error_set(client, 0, "missing_required_field", "missing id");
        return -1;
    }
    return domain_method(client, "POST", domain_id, "/verify", NULL);
}

int resend_domains_remove(resend *client, const char *domain_id) {
    if (!client)
        return -1;
    resend_error_clear(client);
    if (!domain_id || !domain_id[0]) {
        resend_error_set(client, 0, "missing_required_field", "missing id");
        return -1;
    }
    return domain_method(client, "DELETE", domain_id, NULL, NULL);
}
