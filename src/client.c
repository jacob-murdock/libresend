#include "internal.h"

#include <curl/curl.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

static CURLcode curl_init_rc = CURLE_FAILED_INIT;
static pthread_once_t curl_once = PTHREAD_ONCE_INIT;

static void curl_once_init(void) {
    curl_init_rc = curl_global_init(CURL_GLOBAL_DEFAULT);
}

static int ensure_curl(void) {
    pthread_once(&curl_once, curl_once_init);
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
        (void)json_get_string(body, len, "name", &name);
        (void)json_get_string(body, len, "message", &message);
    }
    resend_error_set(client, status, name, message ? message : "");
    free(name);
    free(message);
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
    free(client->api_key);
    free(client->base_url);
    free(client->err_name);
    free(client->err_message);
    free(client);
}

void resend_set_base_url(resend *client, const char *base_url) {
    if (!client || !base_url || !base_url[0])
        return;
    size_t n = strlen(base_url);
    while (n > 0 && base_url[n - 1] == '/')
        n--;
    char *copy = malloc(n + 1);
    if (!copy)
        return;
    memcpy(copy, base_url, n);
    copy[n] = '\0';
    free(client->base_url);
    client->base_url = copy;
}


const resend_error *resend_last_error(const resend *client) {
    if (!client)
        return NULL;
    return &client->err;
}
