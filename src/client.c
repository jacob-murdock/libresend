#include "internal.h"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#endif
#include <curl/curl.h>
#include <openssl/crypto.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <pthread.h>
#endif
#include <stdlib.h>
#include <string.h>

static CURLcode curl_init_rc = CURLE_FAILED_INIT;

static void curl_once_init(void) {
    curl_init_rc = curl_global_init(CURL_GLOBAL_DEFAULT);
}

#ifdef _WIN32
static INIT_ONCE curl_once = INIT_ONCE_STATIC_INIT;

static BOOL CALLBACK curl_once_cb(PINIT_ONCE once, PVOID param, PVOID *ctx) {
    (void)once;
    (void)param;
    (void)ctx;
    curl_once_init();
    return TRUE;
}
#else
static pthread_once_t curl_once = PTHREAD_ONCE_INIT;
#endif

static int ensure_curl(void) {
#ifdef _WIN32
    InitOnceExecuteOnce(&curl_once, curl_once_cb, NULL, NULL);
#else
    pthread_once(&curl_once, curl_once_init);
#endif
    return curl_init_rc == CURLE_OK ? 0 : -1;
}

void resend_error_clear(resend *client) {
    free(client->err_name);
    free(client->err_message);
    client->err_name = NULL;
    client->err_message = NULL;
    client->err.status = 0;
    client->err.name = NULL;
    client->err.message = "";
}

void resend_error_set(resend *client, int status, const char *name, const char *message) {
    resend_error_clear(client);
    client->err.status = status;
    if (name) {
        client->err_name = strdup(name);
        client->err.name = client->err_name;
    }
    if (message && message[0]) {
        client->err_message = strdup(message);
        client->err.message = client->err_message ? client->err_message : "";
    }
}

void resend_error_from_body(resend *client, int status, const char *body, size_t len) {
    char *name = NULL;
    char *message = NULL;
    if (body && len) {
        (void)resend_json_get_string(body, len, "name", &name);
        (void)resend_json_get_string(body, len, "message", &message);
    }
    resend_error_set(client, status, name, message ? message : "");
    free(name);
    free(message);
}

const char *resend_version(void) {
    return LIBRESEND_VERSION;
}

resend *resend_init(const char *api_key) {
    if (!api_key)
        return NULL;
    if (ensure_curl() != 0)
        return NULL;
    resend *client = calloc(1, sizeof *client);
    if (!client)
        return NULL;
    client->api_key = strdup(api_key);
    client->base_url = strdup(LIBRESEND_DEFAULT_BASE);
    if (!client->api_key || !client->base_url) {
        resend_free(client);
        return NULL;
    }
    client->err.message = "";
    return client;
}

void resend_free(resend *client) {
    if (!client)
        return;
    if (client->api_key) {
        OPENSSL_cleanse(client->api_key, strlen(client->api_key));
        free(client->api_key);
    }
    free(client->base_url);
    free(client->err_name);
    free(client->err_message);
    free(client);
}

/* Digits only, and at least one. The authority ends before this is called. */
static int port_digits(const char *p, const char *end) {
    if (p >= end)
        return 0;
    for (; p < end; p++) {
        if (*p < '0' || *p > '9')
            return 0;
    }
    return 1;
}

/*
 * Cleartext http:// is only the loopback fake. The host is the authority
 * after the last '@', so userinfo cannot aim the request at another machine.
 * 127.0.0.1, localhost, or bracketed ::1. A port, when present, is digits.
 */
static int http_loopback(const char *url, size_t n) {
    if (n < 7 || resend_ascii_ncasecmp(url, "http://", 7) != 0)
        return 0;
    const char *auth = url + 7;
    const char *auth_end = url + n;
    for (const char *p = auth; p < auth_end; p++) {
        if (*p == '/' || *p == '?' || *p == '#') {
            auth_end = p;
            break;
        }
    }
    if (auth == auth_end)
        return 0;

    const char *host = auth;
    for (const char *p = auth; p < auth_end; p++) {
        if (*p == '@')
            host = p + 1;
    }
    if (host >= auth_end)
        return 0;

    const char *host_end;
    if (*host == '[') {
        const char *rb = host + 1;
        while (rb < auth_end && *rb != ']')
            rb++;
        if (rb >= auth_end || (size_t)(rb - (host + 1)) != 3 || memcmp(host + 1, "::1", 3) != 0)
            return 0;
        host_end = rb + 1;
    } else {
        host_end = host;
        while (host_end < auth_end && *host_end != ':')
            host_end++;
        size_t hlen = (size_t)(host_end - host);
        int v4 = hlen == 9 && memcmp(host, "127.0.0.1", 9) == 0;
        int local = hlen == 9 && resend_ascii_ncasecmp(host, "localhost", 9) == 0;
        if (!v4 && !local)
            return 0;
    }

    if (host_end == auth_end)
        return 1;
    if (*host_end != ':')
        return 0;
    return port_digits(host_end + 1, auth_end);
}

int resend_set_base_url(resend *client, const char *base_url) {
    if (!client)
        return -1;
    resend_error_clear(client);
    if (!base_url || !base_url[0]) {
        resend_error_set(client, 0, "missing_required_field", "base url is required");
        return -1;
    }
    size_t n = strlen(base_url);
    while (n > 0 && base_url[n - 1] == '/')
        n--;
    if (n == 0) {
        resend_error_set(client, 0, "missing_required_field", "base url is required");
        return -1;
    }
    int https = n >= 8 && resend_ascii_ncasecmp(base_url, "https://", 8) == 0;
    int http = !https && n >= 7 && resend_ascii_ncasecmp(base_url, "http://", 7) == 0;
    if (!https && !http) {
        resend_error_set(client, 0, "validation_error", "base url must be http or https");
        return -1;
    }
    if (http && !http_loopback(base_url, n)) {
        resend_error_set(client, 0, "validation_error", "http base url must be loopback");
        return -1;
    }
    char *copy = malloc(n + 1);
    if (!copy) {
        resend_error_set(client, 0, NULL, "out of memory");
        return -1;
    }
    memcpy(copy, base_url, n);
    copy[n] = '\0';
    free(client->base_url);
    client->base_url = copy;
    return 0;
}


const resend_error *resend_last_error(const resend *client) {
    if (!client)
        return NULL;
    return &client->err;
}
