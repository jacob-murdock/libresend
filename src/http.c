#include "internal.h"

#include <curl/curl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

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

int resend_http_json(resend *client, const char *method, const char *path, const char *body,
                     const char *idempotency_key, resend_http_response *out) {
    memset(out, 0, sizeof *out);
    resend_buf_init(&out->body);

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
    /* System CA store; CURLOPT_CAINFO left unset. */

    CURLcode rc = curl_easy_perform(easy);
    if (hctx.drop_to_http) {
        resend_error_set(client, 0, NULL, "redirect to http refused");
        curl_slist_free_all(headers);
        curl_easy_cleanup(easy);
        free(url);
        resend_http_response_free(out);
        return -1;
    }
    if (wctx.overflow) {
        resend_error_set(client, 0, NULL, "response body too large");
        curl_slist_free_all(headers);
        curl_easy_cleanup(easy);
        free(url);
        resend_http_response_free(out);
        return -1;
    }
    if (rc != CURLE_OK) {
        resend_error_set(client, 0, NULL, curl_easy_strerror(rc));
        curl_slist_free_all(headers);
        curl_easy_cleanup(easy);
        free(url);
        resend_http_response_free(out);
        return -1;
    }

    long status = 0;
    curl_easy_getinfo(easy, CURLINFO_RESPONSE_CODE, &status);
    out->status = status;

    curl_slist_free_all(headers);
    curl_easy_cleanup(easy);
    free(url);
    return 0;
}
