#include "internal.h"

#include <curl/curl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

/* RFC 3986 unreserved characters stay literal. Everything else is %XX. */
static int put_pct(resend_buf *b, const char *s) {
    static const char hex[] = "0123456789ABCDEF";
    if (!s)
        return -1;
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        unsigned char c = *p;
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' ||
            c == '_' || c == '.' || c == '~') {
            if (resend_buf_putc(b, (char)c) != 0)
                return -1;
        } else if (resend_buf_putc(b, '%') != 0 || resend_buf_putc(b, hex[c >> 4]) != 0 ||
                   resend_buf_putc(b, hex[c & 0xf]) != 0) {
            return -1;
        }
    }
    return 0;
}

static int query_param(resend_buf *q, int *first, const char *key, const char *value) {
    if (!value)
        return 0;
    if (*first) {
        if (resend_buf_putc(q, '?') != 0)
            return -1;
        *first = 0;
    } else if (resend_buf_putc(q, '&') != 0) {
        return -1;
    }
    if (resend_buf_puts(q, key) != 0 || resend_buf_putc(q, '=') != 0)
        return -1;
    return put_pct(q, value);
}

int resend_path_with_page(resend *client, const char *prefix, const resend_page *page, char **out) {
    if (!out)
        return -1;
    *out = NULL;
    if (!client || !prefix)
        return -1;
    if (page && page->after && page->before) {
        resend_error_set(client, 0, NULL, "after and before cannot be combined");
        return -1;
    }

    resend_buf path;
    resend_buf_init(&path);
    if (resend_buf_puts(&path, prefix) != 0)
        goto oom;

    if (page) {
        int first = 1;
        if (query_param(&path, &first, "after", page->after) != 0)
            goto oom;
        if (query_param(&path, &first, "before", page->before) != 0)
            goto oom;
        if (page->limit > 0) {
            char lim[32];
            snprintf(lim, sizeof lim, "%zu", page->limit);
            if (query_param(&path, &first, "limit", lim) != 0)
                goto oom;
        }
    }

    if (!path.data)
        goto oom;
    *out = path.data;
    return 0;
oom:
    resend_buf_free(&path);
    resend_error_set(client, 0, NULL, "out of memory");
    return -1;
}

/* Literal `.` and `..` are the real risk: dot is unreserved, so put_pct
 * leaves them as a path segment. `%2e` becomes `%252e` and cannot decode
 * to a dot here; that part is defensive, matching Node. */
static int is_dot_segment(const char *id) {
    int n = 0;
    for (const unsigned char *p = (const unsigned char *)id; *p;) {
        if (*p == '.') {
            n++;
            p++;
        } else if (p[0] == '%' && p[1] == '2' && (p[2] == 'e' || p[2] == 'E')) {
            n++;
            p += 3;
        } else {
            return 0;
        }
        if (n > 2)
            return 0;
    }
    return n == 1 || n == 2;
}

int resend_path_with_id(resend *client, const char *prefix, const char *id, const char *suffix,
                        char **out) {
    if (!out)
        return -1;
    *out = NULL;
    if (!client || !prefix || !id)
        return -1;
    if (is_dot_segment(id)) {
        resend_error_set(client, 0, NULL, "id cannot be . or ..");
        return -1;
    }

    resend_buf path;
    resend_buf_init(&path);
    if (resend_buf_puts(&path, prefix) != 0 || put_pct(&path, id) != 0 ||
        (suffix && suffix[0] && resend_buf_puts(&path, suffix) != 0) || !path.data)
        goto oom;
    *out = path.data;
    return 0;
oom:
    resend_buf_free(&path);
    resend_error_set(client, 0, NULL, "out of memory");
    return -1;
}

void resend_http_response_free(resend_http_response *r) {
    if (!r)
        return;
    resend_buf_free(&r->body);
    r->status = 0;
}

struct write_ctx {
    resend_buf *body;
    int overflow;
};

struct header_ctx {
    int drop_to_http;
};

static size_t write_cb(char *ptr, size_t size, size_t nmemb, void *userdata) {
    struct write_ctx *ctx = userdata;
    size_t n = size * nmemb;
    if (ctx->body->len > LIBRESEND_BODY_MAX || n > LIBRESEND_BODY_MAX - ctx->body->len) {
        ctx->overflow = 1;
        return 0;
    }
    if (resend_buf_put(ctx->body, ptr, n) != 0) {
        ctx->overflow = 1;
        return 0;
    }
    return n;
}

/* Absolute http:// Location is a drop off HTTPS (and refused on HTTP too).
 * Relative and https:// targets are allowed; curl follows those. */
static size_t header_cb(char *buf, size_t size, size_t nitems, void *userdata) {
    struct header_ctx *ctx = userdata;
    size_t n = size * nitems;
    if (n >= 9 && strncasecmp(buf, "Location:", 9) == 0) {
        const char *v = buf + 9;
        const char *end = buf + n;
        while (v < end && (*v == ' ' || *v == '\t'))
            v++;
        if ((size_t)(end - v) >= 7 && strncasecmp(v, "http://", 7) == 0) {
            ctx->drop_to_http = 1;
            return 0;
        }
    }
    return n;
}

/* Curl copies CR and LF from these strings into the request. */
static int no_ctl(const char *s) {
    if (!s)
        return 1;
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        if (*p < 0x20 || *p == 0x7f)
            return 0;
    }
    return 1;
}

int resend_http_json(resend *client, const char *method, const char *path, const char *body,
                     const char *idempotency_key, resend_http_response *out) {
    memset(out, 0, sizeof *out);
    resend_buf_init(&out->body);
    if (!no_ctl(client->base_url)) {
        resend_error_set(client, 0, NULL, "base url contains a control character");
        return -1;
    }
    if (!no_ctl(client->api_key)) {
        resend_error_set(client, 0, NULL, "api key contains a control character");
        return -1;
    }
    if (!no_ctl(idempotency_key)) {
        resend_error_set(client, 0, NULL, "idempotency key contains a control character");
        return -1;
    }

    size_t url_len = strlen(client->base_url) + strlen(path) + 1;
    char *url = malloc(url_len);
    if (!url) {
        resend_error_set(client, 0, NULL, "out of memory");
        return -1;
    }
    snprintf(url, url_len, "%s%s", client->base_url, path);

    CURL *easy = curl_easy_init();
    if (!easy) {
        free(url);
        resend_error_set(client, 0, NULL, "curl_easy_init failed");
        return -1;
    }

    size_t auth_len = strlen("Authorization: Bearer ") + strlen(client->api_key) + 1;
    char *auth = malloc(auth_len);
    char *idem = NULL;
    char ua[64];
    if (!auth) {
        curl_easy_cleanup(easy);
        free(url);
        resend_error_set(client, 0, NULL, "out of memory");
        return -1;
    }
    snprintf(auth, auth_len, "Authorization: Bearer %s", client->api_key);
    snprintf(ua, sizeof ua, "User-Agent: libresend/%s", LIBRESEND_VERSION);
    if (idempotency_key) {
        size_t idem_len = strlen("Idempotency-Key: ") + strlen(idempotency_key) + 1;
        idem = malloc(idem_len);
        if (!idem) {
            free(auth);
            curl_easy_cleanup(easy);
            free(url);
            resend_error_set(client, 0, NULL, "out of memory");
            return -1;
        }
        snprintf(idem, idem_len, "Idempotency-Key: %s", idempotency_key);
    }

    struct curl_slist *headers = NULL;
    headers = curl_slist_append(headers, auth);
    headers = curl_slist_append(headers, ua);
    headers = curl_slist_append(headers, "Accept: application/json");
    headers = curl_slist_append(headers, "Content-Type: application/json");
    if (idem)
        headers = curl_slist_append(headers, idem);
    free(auth);
    free(idem);
    if (!headers) {
        curl_easy_cleanup(easy);
        free(url);
        resend_error_set(client, 0, NULL, "out of memory");
        return -1;
    }

    struct write_ctx wctx = {.body = &out->body, .overflow = 0};
    struct header_ctx hctx = {.drop_to_http = 0};

    curl_easy_setopt(easy, CURLOPT_URL, url);
    /* Ids that are `.` or `..` are already rejected. Keep the path we built. */
    curl_easy_setopt(easy, CURLOPT_PATH_AS_IS, 1L);
    curl_easy_setopt(easy, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(easy, CURLOPT_CUSTOMREQUEST, method);
    if (body) {
        curl_easy_setopt(easy, CURLOPT_POSTFIELDS, body);
        curl_easy_setopt(easy, CURLOPT_POSTFIELDSIZE, (long)strlen(body));
    }
    curl_easy_setopt(easy, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(easy, CURLOPT_WRITEDATA, &wctx);
    curl_easy_setopt(easy, CURLOPT_HEADERFUNCTION, header_cb);
    curl_easy_setopt(easy, CURLOPT_HEADERDATA, &hctx);
    curl_easy_setopt(easy, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(easy, CURLOPT_MAXREDIRS, 10L);
    curl_easy_setopt(easy, CURLOPT_PROTOCOLS_STR, "http,https");
    curl_easy_setopt(easy, CURLOPT_REDIR_PROTOCOLS_STR, "http,https");
    curl_easy_setopt(easy, CURLOPT_TIMEOUT, (long)LIBRESEND_TIMEOUT_SEC);
    curl_easy_setopt(easy, CURLOPT_CONNECTTIMEOUT, (long)LIBRESEND_CONNECT_TIMEOUT_SEC);
    curl_easy_setopt(easy, CURLOPT_NOSIGNAL, 1L);
    /* System CA store; CURLOPT_CAINFO left unset. */

    CURLcode rc = curl_easy_perform(easy);
    long status = 0;
    curl_easy_getinfo(easy, CURLINFO_RESPONSE_CODE, &status);

    curl_slist_free_all(headers);
    curl_easy_cleanup(easy);
    free(url);

    if (hctx.drop_to_http) {
        resend_error_set(client, 0, NULL, "redirect to http refused");
        resend_http_response_free(out);
        return -1;
    }
    if (wctx.overflow) {
        resend_error_set(client, 0, NULL, "response body too large");
        resend_http_response_free(out);
        return -1;
    }
    if (rc != CURLE_OK) {
        resend_error_set(client, 0, NULL, curl_easy_strerror(rc));
        resend_http_response_free(out);
        return -1;
    }

    out->status = status;
    return 0;
}
