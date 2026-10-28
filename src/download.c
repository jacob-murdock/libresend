#include "internal.h"

#include <curl/curl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * Plain GET of a signed URL, like resend-node's downloadRaw. Absolute https://
 * only, redirects included. No Authorization header: the signed URL is the proof.
 * 64 MiB cap. Connect timeout matches API calls; the transfer uses a
 * stall abort instead of a total deadline. *out_bytes is malloc'd.
 */

struct write_ctx {
    resend_buf *body;
    int overflow;
};

static size_t write_cb(char *ptr, size_t size, size_t nmemb, void *userdata) {
    struct write_ctx *ctx = userdata;
    if (size != 0 && nmemb > SIZE_MAX / size) {
        ctx->overflow = 1;
        return 0;
    }
    size_t n = size * nmemb;
    if (ctx->body->len > LIBRESEND_LARGE_BODY_MAX || n > LIBRESEND_LARGE_BODY_MAX - ctx->body->len) {
        ctx->overflow = 1;
        return 0;
    }
    if (resend_buf_put(ctx->body, ptr, n) != 0) {
        ctx->overflow = 1;
        return 0;
    }
    return n;
}

static int https_scheme(const char *url) {
    return url && resend_ascii_ncasecmp(url, "https://", 8) == 0;
}

static int has_ctl(const char *s) {
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        if (*p < 0x20 || *p == 0x7f)
            return 1;
    }
    return 0;
}

int resend_download(resend *client, const char *https_url, void **out_bytes, size_t *out_len) {
    if (out_bytes)
        *out_bytes = NULL;
    if (out_len)
        *out_len = 0;
    if (!client)
        return -1;
    resend_error_clear(client);
    if (!https_url || !https_url[0]) {
        resend_error_set(client, 0, "missing_required_field", "missing url");
        return -1;
    }
    if (!out_bytes || !out_len) {
        resend_error_set(client, 0, "missing_required_field", "missing result");
        return -1;
    }

    if (has_ctl(https_url)) {
        resend_error_set(client, 0, "validation_error", "invalid download url");
        return -1;
    }
    if (!https_scheme(https_url)) {
        resend_error_set(client, 0, "validation_error", "download url must be https");
        return -1;
    }

    CURL *easy = curl_easy_init();
    if (!easy) {
        resend_error_set(client, 0, NULL, "curl_easy_init failed");
        return -1;
    }

    char ua[64];
    snprintf(ua, sizeof ua, "User-Agent: libresend/%s", LIBRESEND_VERSION);
    struct curl_slist *headers = NULL;
    headers = curl_slist_append(headers, ua);
    if (!headers) {
        curl_easy_cleanup(easy);
        resend_error_set(client, 0, NULL, "out of memory");
        return -1;
    }

    resend_buf body;
    resend_buf_init(&body);
    struct write_ctx wctx = {.body = &body, .overflow = 0};

    curl_easy_setopt(easy, CURLOPT_URL, https_url);
    curl_easy_setopt(easy, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(easy, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(easy, CURLOPT_WRITEDATA, &wctx);
    curl_easy_setopt(easy, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(easy, CURLOPT_MAXREDIRS, 10L);
    curl_easy_setopt(easy, CURLOPT_CONNECTTIMEOUT, (long)LIBRESEND_CONNECT_TIMEOUT_SEC);
    curl_easy_setopt(easy, CURLOPT_NOSIGNAL, 1L);
    /* Abort if the transfer stays under 1 byte/s for 30s. No total deadline. */
    curl_easy_setopt(easy, CURLOPT_LOW_SPEED_LIMIT, 1L);
    curl_easy_setopt(easy, CURLOPT_LOW_SPEED_TIME, (long)LIBRESEND_TIMEOUT_SEC);
    if (resend_curl_set_protocols(easy, 1) != 0 ||
        curl_easy_setopt(easy, CURLOPT_SSL_VERIFYPEER, 1L) != CURLE_OK ||
        curl_easy_setopt(easy, CURLOPT_SSL_VERIFYHOST, 2L) != CURLE_OK) {
        curl_slist_free_all(headers);
        curl_easy_cleanup(easy);
        resend_error_set(client, 0, NULL, "curl setup failed");
        return -1;
    }

    CURLcode rc = curl_easy_perform(easy);
    long status = 0;
    curl_easy_getinfo(easy, CURLINFO_RESPONSE_CODE, &status);

    curl_slist_free_all(headers);
    curl_easy_cleanup(easy);

    if (wctx.overflow) {
        resend_buf_free(&body);
        resend_error_set(client, 0, NULL, "response body too large");
        return -1;
    }
    if (rc != CURLE_OK) {
        resend_buf_free(&body);
        resend_error_set(client, 0, NULL, curl_easy_strerror(rc));
        return -1;
    }
    if (status < 200 || status >= 300) {
        char msg[64];
        snprintf(msg, sizeof msg, "download failed: HTTP %ld", status);
        resend_error_set(client, (int)status, NULL, msg);
        resend_buf_free(&body);
        return -1;
    }

    *out_bytes = body.data ? body.data : calloc(1, 1);
    if (!*out_bytes) {
        resend_buf_free(&body);
        resend_error_set(client, 0, NULL, "out of memory");
        return -1;
    }
    *out_len = body.data ? body.len : 0;
    /* Caller owns the buffer; do not free body.data. */
    return 0;
}
