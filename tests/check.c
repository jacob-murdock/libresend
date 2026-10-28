/*
 * libresend suite. The compiler and main are the runner.
 * CHECK counts misses and keeps going. The process exits non-zero
 * when the count is not zero. This file includes the public header
 * only; the fake is the wire.
 */

#include "resend.h"

#include "fake.h"

#include <openssl/evp.h>
#include <openssl/hmac.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static int failures;

#define CHECK(...)                                                                 \
    do {                                                                           \
        if (!(__VA_ARGS__)) {                                                      \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #__VA_ARGS__); \
            failures++;                                                            \
        }                                                                          \
    } while (0)

static int header_eq(fake *f, const char *name, const char *value) {
    const char *got = fake_header(f, name);
    return got != NULL && strcmp(got, value) == 0;
}

static int header_prefix(fake *f, const char *name, const char *prefix) {
    const char *got = fake_header(f, name);
    return got != NULL && strncmp(got, prefix, strlen(prefix)) == 0;
}

static int body_eq(fake *f, const char *want) {
    const char *got = fake_body(f);
    return got != NULL && strcmp(got, want) == 0;
}

static resend_send smoke_message(const char *const *to) {
    resend_send message = {
        .from = "Acme <hello@example.com>",
        .to = to,
        .to_count = 1,
        .subject = "Hello",
        .text = "Hello from C.",
    };
    return message;
}

static void test_send(void) {
    static const char id[] = "49a3999c-0ce1-4ea6-ab68-afcd6dc2e794";
    char raw[64];
    snprintf(raw, sizeof raw, "{\"id\":\"%s\"}", id);

    fake_exchange exchange = {
        .status = 200,
        .body = raw,
    };
    fake *f = fake_start(&exchange, 1);
    CHECK(f != NULL);
    if (!f)
        return;

    char base[64];
    snprintf(base, sizeof base, "http://127.0.0.1:%d", fake_port(f));

    const char *const to[] = {"you@example.com"};
    resend_send message = smoke_message(to);
    resend_sent sent = {0};
    int rc = -1;

    resend *client = resend_init("re_test");
    CHECK(client != NULL);
    if (client) {
        CHECK(resend_set_base_url(client, base) == 0);
        rc = resend_emails_send(client, &message, &sent);
    }

    CHECK(rc == 0);
    CHECK(sent.id != NULL && strcmp(sent.id, id) == 0);
    CHECK(fake_request_count(f) == 1);
    CHECK(fake_method(f) != NULL && strcmp(fake_method(f), "POST") == 0);
    CHECK(fake_path(f) != NULL && strcmp(fake_path(f), "/emails") == 0);
    CHECK(header_eq(f, "Authorization", "Bearer re_test"));
    CHECK(header_prefix(f, "User-Agent", "libresend/"));
    CHECK(header_eq(f, "Accept", "application/json"));
    CHECK(header_eq(f, "Content-Type", "application/json"));
    CHECK(fake_body(f) != NULL &&
          strcmp(fake_body(f),
                 "{\"from\":\"Acme <hello@example.com>\",\"to\":[\"you@example.com\"],"
                 "\"subject\":\"Hello\",\"text\":\"Hello from C.\"}") == 0);

    resend_sent_free(&sent);
    resend_free(client);
    fake_stop(f);
}

/* Empty or NULL leaves the stored URL. A later send still hits the fake. */
static void test_set_base_url(void) {
    CHECK(resend_set_base_url(NULL, "https://example.com") == -1);

    static const char id[] = "49a3999c-0ce1-4ea6-ab68-afcd6dc2e794";
    char raw[64];
    snprintf(raw, sizeof raw, "{\"id\":\"%s\"}", id);

    fake_exchange exchange = {
        .status = 200,
        .body = raw,
    };
    fake *f = fake_start(&exchange, 1);
    CHECK(f != NULL);
    if (!f)
        return;

    char base[64];
    snprintf(base, sizeof base, "http://127.0.0.1:%d", fake_port(f));

    resend *client = resend_init("re_test");
    CHECK(client != NULL);
    if (!client) {
        fake_stop(f);
        return;
    }

    CHECK(resend_set_base_url(client, NULL) == -1);
    const resend_error *err = resend_last_error(client);
    CHECK(err != NULL && err->status == 0);
    CHECK(err->message != NULL && strcmp(err->message, "base url is required") == 0);

    CHECK(resend_set_base_url(client, "") == -1);
    err = resend_last_error(client);
    CHECK(err != NULL && err->message != NULL && strcmp(err->message, "base url is required") == 0);

    CHECK(resend_set_base_url(client, base) == 0);
    CHECK(resend_set_base_url(client, "") == -1);
    err = resend_last_error(client);
    CHECK(err != NULL && err->message != NULL && strcmp(err->message, "base url is required") == 0);

    CHECK(resend_set_base_url(client, "file:///etc/passwd") == -1);
    err = resend_last_error(client);
    CHECK(err != NULL && err->status == 0);
    CHECK(err->message != NULL && strcmp(err->message, "base url must be http or https") == 0);

    CHECK(resend_set_base_url(client, "gopher://127.0.0.1/1") == -1);
    err = resend_last_error(client);
    CHECK(err != NULL && err->message != NULL &&
          strcmp(err->message, "base url must be http or https") == 0);

    CHECK(resend_set_base_url(client, "/") == -1);
    err = resend_last_error(client);
    CHECK(err != NULL && err->message != NULL && strcmp(err->message, "base url is required") == 0);

    CHECK(resend_set_base_url(client, "///") == -1);
    err = resend_last_error(client);
    CHECK(err != NULL && err->message != NULL && strcmp(err->message, "base url is required") == 0);
    CHECK(fake_request_count(f) == 0);

    const char *const to[] = {"you@example.com"};
    resend_send message = smoke_message(to);
    resend_sent sent = {0};
    int rc = resend_emails_send(client, &message, &sent);
    CHECK(rc == 0);
    CHECK(sent.id != NULL && strcmp(sent.id, id) == 0);
    CHECK(fake_request_count(f) == 1);

    resend_sent_free(&sent);
    resend_free(client);
    fake_stop(f);
}

static void test_error_body(void) {
    static const char body[] = "{\"statusCode\":422,\"name\":\"validation_error\","
                               "\"message\":\"Invalid `from` field\"}";
    fake_exchange exchange = {
        .status = 422,
        .body = body,
    };
    fake *f = fake_start(&exchange, 1);
    CHECK(f != NULL);
    if (!f)
        return;

    char base[64];
    snprintf(base, sizeof base, "http://127.0.0.1:%d", fake_port(f));

    const char *const to[] = {"you@example.com"};
    resend_send message = {
        .from = "not-an-email",
        .to = to,
        .to_count = 1,
        .subject = "Hello",
        .text = "Hello from C.",
    };
    resend_sent sent = {0};

    resend *client = resend_init("re_test");
    CHECK(client != NULL);
    if (!client) {
        fake_stop(f);
        return;
    }

    CHECK(resend_set_base_url(client, base) == 0);
    int rc = resend_emails_send(client, &message, &sent);
    CHECK(rc == -1);
    CHECK(sent.id == NULL);

    const resend_error *err = resend_last_error(client);
    CHECK(err != NULL);
    CHECK(err->status == 422);
    CHECK(err->name != NULL && strcmp(err->name, "validation_error") == 0);
    CHECK(err->message != NULL && strcmp(err->message, "Invalid `from` field") == 0);

    /* Pointers stay valid until the next call on this client. */
    const char *name = err->name;
    const char *msg = err->message;
    CHECK(resend_last_error(client)->name == name);
    CHECK(resend_last_error(client)->message == msg);
    CHECK(strcmp(name, "validation_error") == 0);
    CHECK(strcmp(msg, "Invalid `from` field") == 0);

    CHECK(resend_set_base_url(client, "http://127.0.0.1:1") == 0);
    rc = resend_emails_send(client, &message, &sent);
    CHECK(rc == -1);
    err = resend_last_error(client);
    CHECK(err != NULL && err->status == 0);
    CHECK(err->name != name);
    CHECK(err->message != msg);

    resend_sent_free(&sent);
    resend_free(client);
    fake_stop(f);
}

/*
 * A bad argument must clear the previous API failure from the slot before
 * rejecting. A NULL client has no slot. set_base_url also clears on success.
 */
static void test_error_clear_on_reject(void) {
    static const char body[] = "{\"statusCode\":422,\"name\":\"validation_error\","
                               "\"message\":\"Invalid `from` field\"}";
    fake_exchange exchange = {.status = 422, .body = body};
    fake *f = fake_start(&exchange, 1);
    CHECK(f != NULL);
    if (!f)
        return;

    char base[64];
    snprintf(base, sizeof base, "http://127.0.0.1:%d", fake_port(f));

    const char *const to[] = {"you@example.com"};
    resend_send message = {
        .from = "not-an-email",
        .to = to,
        .to_count = 1,
        .subject = "Hello",
        .text = "Hello from C.",
    };
    resend_sent sent = {0};
    resend *client = resend_init("re_test");
    CHECK(client != NULL);
    if (!client) {
        fake_stop(f);
        return;
    }
    CHECK(resend_set_base_url(client, base) == 0);

    CHECK(resend_emails_send(client, &message, &sent) == -1);
    const resend_error *err = resend_last_error(client);
    CHECK(err != NULL && err->status == 422);
    CHECK(err->name != NULL && strcmp(err->name, "validation_error") == 0);

    CHECK(resend_emails_send(NULL, &message, &sent) == -1);

    CHECK(resend_emails_send(client, NULL, &sent) == -1);
    err = resend_last_error(client);
    CHECK(err != NULL && err->status == 0);
    CHECK(err->name != NULL && strcmp(err->name, "missing_required_field") == 0);
    CHECK(err->message != NULL && strcmp(err->message, "missing request") == 0);
    CHECK(fake_request_count(f) == 1);

    CHECK(resend_emails_send(client, &message, &sent) == -1);
    err = resend_last_error(client);
    CHECK(err != NULL && err->status == 422);

    CHECK(resend_emails_update(client, "id", NULL, &sent) == -1);
    err = resend_last_error(client);
    CHECK(err != NULL && err->status == 0);
    CHECK(err->name != NULL && strcmp(err->name, "missing_required_field") == 0);
    CHECK(err->message != NULL && strcmp(err->message, "missing scheduled_at") == 0);
    CHECK(fake_request_count(f) == 2);

    CHECK(resend_emails_send(client, &message, &sent) == -1);
    err = resend_last_error(client);
    CHECK(err != NULL && err->status == 422);

    resend_suppression s = {0};
    CHECK(resend_suppressions_get(client, NULL, &s) == -1);
    err = resend_last_error(client);
    CHECK(err != NULL && err->status == 0);
    CHECK(err->name != NULL && strcmp(err->name, "missing_required_field") == 0);
    CHECK(err->message != NULL && strcmp(err->message, "missing id") == 0);
    CHECK(fake_request_count(f) == 3);

    CHECK(resend_emails_send(client, &message, &sent) == -1);
    err = resend_last_error(client);
    CHECK(err != NULL && err->status == 422);

    CHECK(resend_emails_list(client, NULL, NULL) == -1);
    err = resend_last_error(client);
    CHECK(err->name != NULL && strcmp(err->name, "missing_required_field") == 0);
    CHECK(err->message != NULL && strcmp(err->message, "missing result") == 0);

    CHECK(resend_emails_send(client, &message, &sent) == -1);
    err = resend_last_error(client);
    CHECK(err != NULL && err->status == 422);

    resend_received r = {0};
    CHECK(resend_receiving_get(client, "id", "markdown", &r) == -1);
    err = resend_last_error(client);
    CHECK(err != NULL && err->status == 0);
    CHECK(err->name != NULL && strcmp(err->name, "validation_error") == 0);
    CHECK(fake_request_count(f) == 5);

    CHECK(resend_set_base_url(client, "http://127.0.0.1:1") == 0);
    err = resend_last_error(client);
    CHECK(err != NULL && err->status == 0);
    CHECK(err->name == NULL);
    CHECK(err->message != NULL && err->message[0] == '\0');

    resend_sent_free(&sent);
    resend_free(client);
    fake_stop(f);
}

/* Nothing listens on port 1; the call must fail before an HTTP status. */
static void test_transport_failure(void) {
    const char *const to[] = {"you@example.com"};
    resend_send message = smoke_message(to);
    resend_sent sent = {0};

    resend *client = resend_init("re_test");
    CHECK(client != NULL);
    if (!client)
        return;

    CHECK(resend_set_base_url(client, "http://127.0.0.1:1") == 0);
    int rc = resend_emails_send(client, &message, &sent);
    CHECK(rc == -1);
    CHECK(sent.id == NULL);

    const resend_error *err = resend_last_error(client);
    CHECK(err != NULL);
    CHECK(err->status == 0);
    CHECK(err->message != NULL);

    resend_sent_free(&sent);
    resend_free(client);
}

static void test_body_cap(void) {
    enum { oversized = 1024 * 1024 + 1 };
    char *raw = malloc(oversized + 16);
    CHECK(raw != NULL);
    if (!raw)
        return;
    memset(raw, 'a', oversized);
    raw[0] = '{';
    raw[1] = '"';
    raw[2] = 'x';
    raw[3] = '"';
    raw[4] = ':';
    raw[5] = '"';
    raw[oversized - 2] = '"';
    raw[oversized - 1] = '}';
    raw[oversized] = '\0';

    fake_exchange exchange = {
        .status = 200,
        .body = raw,
    };
    fake *f = fake_start(&exchange, 1);
    CHECK(f != NULL);
    if (!f) {
        free(raw);
        return;
    }

    char base[64];
    snprintf(base, sizeof base, "http://127.0.0.1:%d", fake_port(f));

    const char *const to[] = {"you@example.com"};
    resend_send message = smoke_message(to);
    resend_sent sent = {0};

    resend *client = resend_init("re_test");
    CHECK(client != NULL);
    if (!client) {
        fake_stop(f);
        free(raw);
        return;
    }

    CHECK(resend_set_base_url(client, base) == 0);
    int rc = resend_emails_send(client, &message, &sent);
    CHECK(rc == -1);
    CHECK(sent.id == NULL);
    const resend_error *err = resend_last_error(client);
    CHECK(err != NULL && err->status == 0);

    resend_sent_free(&sent);
    resend_free(client);
    fake_stop(f);
    free(raw);
}

/* Relative Location stays on the fake; the client follows to the 200. */
static void test_redirect_follow(void) {
    static const char id[] = "49a3999c-0ce1-4ea6-ab68-afcd6dc2e794";
    char raw[64];
    snprintf(raw, sizeof raw, "{\"id\":\"%s\"}", id);

    fake_exchange script[] = {
        {.status = 302, .body = "", .location = "/emails"},
        {.status = 200, .body = raw},
    };
    fake *f = fake_start(script, 2);
    CHECK(f != NULL);
    if (!f)
        return;

    char base[64];
    snprintf(base, sizeof base, "http://127.0.0.1:%d", fake_port(f));

    const char *const to[] = {"you@example.com"};
    resend_send message = smoke_message(to);
    resend_sent sent = {0};

    resend *client = resend_init("re_test");
    CHECK(client != NULL);
    if (!client) {
        fake_stop(f);
        return;
    }

    CHECK(resend_set_base_url(client, base) == 0);
    int rc = resend_emails_send(client, &message, &sent);
    CHECK(rc == 0);
    CHECK(sent.id != NULL && strcmp(sent.id, id) == 0);
    CHECK(fake_request_count(f) == 2);

    resend_sent_free(&sent);
    resend_free(client);
    fake_stop(f);
}

/*
 * 127.0.0.1 and localhost are different hosts. libcurl strips the custom
 * Authorization header on that hop unless CURLOPT_UNRESTRICTED_AUTH is set.
 * A scheme-relative Location stays on HTTP, so the header callback that
 * refuses an absolute http:// downgrade does not abort this follow.
 */
static void test_redirect_other_host(void) {
    static const char id[] = "49a3999c-0ce1-4ea6-ab68-afcd6dc2e794";
    char raw[64];
    snprintf(raw, sizeof raw, "{\"id\":\"%s\"}", id);

    fake_exchange dest_ex = {.status = 200, .body = raw};
    fake *dest = fake_start(&dest_ex, 1);
    CHECK(dest != NULL);
    if (!dest)
        return;

    char location[64];
    snprintf(location, sizeof location, "//localhost:%d/emails", fake_port(dest));

    fake_exchange src_ex = {.status = 302, .body = "", .location = location};
    fake *src = fake_start(&src_ex, 1);
    CHECK(src != NULL);
    if (!src) {
        fake_stop(dest);
        return;
    }

    char base[64];
    snprintf(base, sizeof base, "http://127.0.0.1:%d", fake_port(src));

    const char *const to[] = {"you@example.com"};
    resend_send message = smoke_message(to);
    resend_sent sent = {0};

    resend *client = resend_init("re_test");
    CHECK(client != NULL);
    if (!client) {
        fake_stop(src);
        fake_stop(dest);
        return;
    }

    CHECK(resend_set_base_url(client, base) == 0);
    int rc = resend_emails_send(client, &message, &sent);
    CHECK(rc == 0);
    CHECK(sent.id != NULL && strcmp(sent.id, id) == 0);
    CHECK(fake_request_count(src) == 1);
    CHECK(header_eq(src, "Authorization", "Bearer re_test"));
    CHECK(fake_request_count(dest) == 1);
    CHECK(fake_header(dest, "Authorization") == NULL);

    resend_sent_free(&sent);
    resend_free(client);
    fake_stop(src);
    fake_stop(dest);
}

/* Absolute http:// aimed at this fake. A follow would parse the id.
 * The fake is plain HTTP; this is the https-to-http stand-in. */
static void test_redirect_drop_http(void) {
    static const char id[] = "49a3999c-0ce1-4ea6-ab68-afcd6dc2e794";
    char raw[64];
    snprintf(raw, sizeof raw, "{\"id\":\"%s\"}", id);

    fake_exchange script[] = {
        {.status = 302, .body = "", .location = "http://127.0.0.1:0/emails"},
        {.status = 200, .body = raw},
    };
    fake *f = fake_start(script, 2);
    CHECK(f != NULL);
    if (!f)
        return;

    char base[64];
    snprintf(base, sizeof base, "http://127.0.0.1:%d", fake_port(f));

    const char *const to[] = {"you@example.com"};
    resend_send message = smoke_message(to);
    resend_sent sent = {0};

    resend *client = resend_init("re_test");
    CHECK(client != NULL);
    if (!client) {
        fake_stop(f);
        return;
    }

    CHECK(resend_set_base_url(client, base) == 0);
    int rc = resend_emails_send(client, &message, &sent);
    CHECK(rc == -1);
    CHECK(sent.id == NULL);
    CHECK(fake_request_count(f) == 1);
    const resend_error *err = resend_last_error(client);
    CHECK(err != NULL && err->status == 0);

    resend_sent_free(&sent);
    resend_free(client);
    fake_stop(f);
}

/* Location on a 2xx is not a redirect. The send already happened. */
static void test_location_on_success(void) {
    static const char id[] = "49a3999c-0ce1-4ea6-ab68-afcd6dc2e794";
    char raw[64];
    snprintf(raw, sizeof raw, "{\"id\":\"%s\"}", id);

    fake_exchange exchange = {
        .status = 200,
        .body = raw,
        .location = "http://127.0.0.1:0/emails",
    };
    fake *f = fake_start(&exchange, 1);
    CHECK(f != NULL);
    if (!f)
        return;

    char base[64];
    snprintf(base, sizeof base, "http://127.0.0.1:%d", fake_port(f));

    const char *const to[] = {"you@example.com"};
    resend_send message = smoke_message(to);
    resend_sent sent = {0};

    resend *client = resend_init("re_test");
    CHECK(client != NULL);
    if (!client) {
        fake_stop(f);
        return;
    }

    CHECK(resend_set_base_url(client, base) == 0);
    int rc = resend_emails_send(client, &message, &sent);
    CHECK(rc == 0);
    CHECK(sent.id != NULL && strcmp(sent.id, id) == 0);
    CHECK(fake_request_count(f) == 1);

    resend_sent_free(&sent);
    resend_free(client);
    fake_stop(f);
}

/* A non-2xx keeps its status when it also carries Location: http://. */
static void test_location_on_error(void) {
    static const char body[] = "{\"name\":\"validation_error\","
                               "\"message\":\"Invalid `from` field\"}";
    fake_exchange exchange = {
        .status = 422,
        .body = body,
        .location = "http://127.0.0.1:0/emails",
    };
    fake *f = fake_start(&exchange, 1);
    CHECK(f != NULL);
    if (!f)
        return;

    char base[64];
    snprintf(base, sizeof base, "http://127.0.0.1:%d", fake_port(f));

    const char *const to[] = {"you@example.com"};
    resend_send message = smoke_message(to);
    resend_sent sent = {0};

    resend *client = resend_init("re_test");
    CHECK(client != NULL);
    if (!client) {
        fake_stop(f);
        return;
    }

    CHECK(resend_set_base_url(client, base) == 0);
    int rc = resend_emails_send(client, &message, &sent);
    CHECK(rc == -1);
    CHECK(sent.id == NULL);
    CHECK(fake_request_count(f) == 1);
    const resend_error *err = resend_last_error(client);
    CHECK(err != NULL && err->status == 422);
    CHECK(err->name != NULL && strcmp(err->name, "validation_error") == 0);
    CHECK(err->message != NULL && strcmp(err->message, "Invalid `from` field") == 0);

    resend_sent_free(&sent);
    resend_free(client);
    fake_stop(f);
}

/* Optional send fields land in the JSON body; NULL / count 0 omits. */
static void test_send_fields(void) {
    static const char id[] = "49a3999c-0ce1-4ea6-ab68-afcd6dc2e794";
    char raw[64];
    snprintf(raw, sizeof raw, "{\"id\":\"%s\"}", id);

    fake_exchange exchange = {
        .status = 200,
        .body = raw,
    };
    fake *f = fake_start(&exchange, 1);
    CHECK(f != NULL);
    if (!f)
        return;

    char base[64];
    snprintf(base, sizeof base, "http://127.0.0.1:%d", fake_port(f));

    const char *const to[] = {"you@example.com"};
    const char *const cc[] = {"cc@example.com"};
    const char *const bcc[] = {"bcc@example.com", "bcc2@example.com"};
    const char *const reply_to[] = {"reply@example.com"};
    const resend_pair headers[] = {
        {.name = "X-Entity-Ref-ID", .value = "123"},
        {.name = "X-Custom", .value = "value"},
    };
    const resend_pair tags[] = {
        {.name = "category", .value = "confirm_email"},
    };
    resend_send message = {
        .from = "Acme <hello@example.com>",
        .to = to,
        .to_count = 1,
        .cc = cc,
        .cc_count = 1,
        .bcc = bcc,
        .bcc_count = 2,
        .reply_to = reply_to,
        .reply_to_count = 1,
        .subject = "Hello",
        .html = "<p>Hi</p>",
        .text = "Hi",
        .headers = headers,
        .header_count = 2,
        .tags = tags,
        .tag_count = 1,
        .scheduled_at = "in 1 min",
    };
    resend_sent sent = {0};

    resend *client = resend_init("re_test");
    CHECK(client != NULL);
    if (!client) {
        fake_stop(f);
        return;
    }

    CHECK(resend_set_base_url(client, base) == 0);
    int rc = resend_emails_send(client, &message, &sent);
    CHECK(rc == 0);
    CHECK(sent.id != NULL && strcmp(sent.id, id) == 0);
    CHECK(fake_body(f) != NULL &&
          strcmp(fake_body(f),
                 "{\"from\":\"Acme <hello@example.com>\","
                 "\"to\":[\"you@example.com\"],"
                 "\"cc\":[\"cc@example.com\"],"
                 "\"bcc\":[\"bcc@example.com\",\"bcc2@example.com\"],"
                 "\"reply_to\":[\"reply@example.com\"],"
                 "\"subject\":\"Hello\","
                 "\"html\":\"<p>Hi</p>\","
                 "\"text\":\"Hi\","
                 "\"headers\":{\"X-Entity-Ref-ID\":\"123\",\"X-Custom\":\"value\"},"
                 "\"tags\":[{\"name\":\"category\",\"value\":\"confirm_email\"}],"
                 "\"scheduled_at\":\"in 1 min\"}") == 0);

    resend_sent_free(&sent);
    resend_free(client);
    fake_stop(f);
}

/* html alone is enough; text may be omitted. */
static void test_send_html_only(void) {
    static const char id[] = "49a3999c-0ce1-4ea6-ab68-afcd6dc2e794";
    char raw[64];
    snprintf(raw, sizeof raw, "{\"id\":\"%s\"}", id);

    fake_exchange exchange = {
        .status = 200,
        .body = raw,
    };
    fake *f = fake_start(&exchange, 1);
    CHECK(f != NULL);
    if (!f)
        return;

    char base[64];
    snprintf(base, sizeof base, "http://127.0.0.1:%d", fake_port(f));

    const char *const to[] = {"you@example.com"};
    resend_send message = {
        .from = "Acme <hello@example.com>",
        .to = to,
        .to_count = 1,
        .subject = "Hello",
        .html = "<p>Hi</p>",
    };
    resend_sent sent = {0};

    resend *client = resend_init("re_test");
    CHECK(client != NULL);
    if (!client) {
        fake_stop(f);
        return;
    }

    CHECK(resend_set_base_url(client, base) == 0);
    int rc = resend_emails_send(client, &message, &sent);
    CHECK(rc == 0);
    CHECK(fake_body(f) != NULL &&
          strcmp(fake_body(f),
                 "{\"from\":\"Acme <hello@example.com>\","
                 "\"to\":[\"you@example.com\"],"
                 "\"subject\":\"Hello\","
                 "\"html\":\"<p>Hi</p>\"}") == 0);

    resend_sent_free(&sent);
    resend_free(client);
    fake_stop(f);
}

static double double_bits(uint64_t bits) {
    double n;
    memcpy(&n, &bits, sizeof n);
    return n;
}

/* Template id, string and number variables, and topic_id. html is sent too.
 * NULL variables omits that key. A count with a NULL pointer, and a non-finite
 * number, send nothing. */
static void test_send_template(void) {
    static const char id[] = "49a3999c-0ce1-4ea6-ab68-afcd6dc2e794";
    char raw[64];
    snprintf(raw, sizeof raw, "{\"id\":\"%s\"}", id);

    fake_exchange exchange = {.status = 200, .body = raw};
    fake *f = fake_start(&exchange, 1);
    CHECK(f != NULL);
    if (!f)
        return;

    char base[64];
    snprintf(base, sizeof base, "http://127.0.0.1:%d", fake_port(f));

    const char *const to[] = {"you@example.com"};
    resend_template missing = {.id = "tmpl_123", .variable_count = 1};
    resend_send message = {
        .from = "Acme <hello@example.com>",
        .to = to,
        .to_count = 1,
        .subject = "Hello",
        .html = "<p>Hi</p>",
        .template = &missing,
    };
    resend_sent sent = {0};

    resend *client = resend_init("re_test");
    CHECK(client != NULL);
    if (!client) {
        fake_stop(f);
        return;
    }
    CHECK(resend_set_base_url(client, base) == 0);

    CHECK(resend_emails_send(client, &message, &sent) == -1);
    CHECK(sent.id == NULL);
    CHECK(fake_request_count(f) == 0);
    const resend_error *err = resend_last_error(client);
    CHECK(err != NULL && err->status == 0);
    CHECK(err->name != NULL && strcmp(err->name, "missing_required_field") == 0);
    CHECK(err->message != NULL && strcmp(err->message, "missing variables") == 0);

    const resend_template_variable nan_var[] = {
        {.name = "COUNT", .number = double_bits(0x7ff8000000000000ULL), .as_number = 1},
    };
    resend_template bad = {.id = "tmpl_123", .variables = nan_var, .variable_count = 1};
    message.template = &bad;
    CHECK(resend_emails_send(client, &message, &sent) == -1);
    CHECK(fake_request_count(f) == 0);
    err = resend_last_error(client);
    CHECK(err != NULL && err->status == 0);
    CHECK(err->name != NULL && strcmp(err->name, "validation_error") == 0);
    CHECK(err->message != NULL &&
          strcmp(err->message, "template variable is not a finite number") == 0);

    const resend_template_variable inf_var[] = {
        {.name = "COUNT", .number = double_bits(0x7ff0000000000000ULL), .as_number = 1},
    };
    bad.variables = inf_var;
    CHECK(resend_emails_send(client, &message, &sent) == -1);
    CHECK(fake_request_count(f) == 0);
    err = resend_last_error(client);
    CHECK(err->message != NULL &&
          strcmp(err->message, "template variable is not a finite number") == 0);

    const resend_template_variable vars[] = {
        {.name = "variableName", .value = "Sign up now"},
        {.name = "variableName2", .number = 123, .as_number = 1},
        {.name = "price", .number = 1.5, .as_number = 1},
    };
    resend_template tpl = {.id = "tmpl_123", .variables = vars, .variable_count = 3};
    message.template = &tpl;
    message.topic_id = "b6d24b8e-af0b-4c3c-be0c-359bbd97381e";
    int rc = resend_emails_send(client, &message, &sent);
    CHECK(rc == 0);
    CHECK(sent.id != NULL && strcmp(sent.id, id) == 0);
    CHECK(fake_request_count(f) == 1);
    CHECK(body_eq(f, "{\"from\":\"Acme <hello@example.com>\","
                     "\"to\":[\"you@example.com\"],"
                     "\"subject\":\"Hello\","
                     "\"html\":\"<p>Hi</p>\","
                     "\"template\":{\"id\":\"tmpl_123\","
                     "\"variables\":{\"variableName\":\"Sign up now\","
                     "\"variableName2\":123,\"price\":1.5}},"
                     "\"topic_id\":\"b6d24b8e-af0b-4c3c-be0c-359bbd97381e\"}"));

    resend_sent_free(&sent);
    fake_stop(f);

    f = fake_start(&exchange, 1);
    CHECK(f != NULL);
    if (!f) {
        resend_free(client);
        return;
    }
    snprintf(base, sizeof base, "http://127.0.0.1:%d", fake_port(f));
    CHECK(resend_set_base_url(client, base) == 0);

    resend_template id_only = {.id = "tmpl_123"};
    message.template = &id_only;
    message.topic_id = NULL;
    message.html = NULL;
    sent.id = NULL;
    rc = resend_emails_send(client, &message, &sent);
    CHECK(rc == 0);
    CHECK(fake_request_count(f) == 1);
    CHECK(body_eq(f, "{\"from\":\"Acme <hello@example.com>\","
                     "\"to\":[\"you@example.com\"],"
                     "\"subject\":\"Hello\","
                     "\"template\":{\"id\":\"tmpl_123\"}}"));

    resend_sent_free(&sent);
    resend_free(client);
    fake_stop(f);
}

/* Raw content is base64; path is the other form. Optional type and cid. */
static void test_send_attachments(void) {
    static const char id[] = "49a3999c-0ce1-4ea6-ab68-afcd6dc2e794";
    char raw[64];
    snprintf(raw, sizeof raw, "{\"id\":\"%s\"}", id);

    fake_exchange exchange = {
        .status = 200,
        .body = raw,
    };
    fake *f = fake_start(&exchange, 1);
    CHECK(f != NULL);
    if (!f)
        return;

    char base[64];
    snprintf(base, sizeof base, "http://127.0.0.1:%d", fake_port(f));

    static const unsigned char hello[] = "hello";
    const char *const to[] = {"you@example.com"};
    const resend_attachment attachments[] = {
        {
            .filename = "hello.txt",
            .content = hello,
            .content_len = 5,
            .content_type = "text/plain",
            .content_id = "cid-hello",
        },
        {
            .filename = "doc.pdf",
            .path = "https://example.com/doc.pdf",
        },
    };
    resend_send message = {
        .from = "Acme <hello@example.com>",
        .to = to,
        .to_count = 1,
        .subject = "Hello",
        .text = "Hi",
        .attachments = attachments,
        .attachment_count = 2,
    };
    resend_sent sent = {0};

    resend *client = resend_init("re_test");
    CHECK(client != NULL);
    if (!client) {
        fake_stop(f);
        return;
    }

    CHECK(resend_set_base_url(client, base) == 0);
    int rc = resend_emails_send(client, &message, &sent);
    CHECK(rc == 0);
    CHECK(sent.id != NULL && strcmp(sent.id, id) == 0);
    /* "hello" -> aGVsbG8= */
    CHECK(fake_body(f) != NULL &&
          strcmp(fake_body(f),
                 "{\"from\":\"Acme <hello@example.com>\","
                 "\"to\":[\"you@example.com\"],"
                 "\"subject\":\"Hello\","
                 "\"text\":\"Hi\","
                 "\"attachments\":["
                 "{\"filename\":\"hello.txt\",\"content\":\"aGVsbG8=\","
                 "\"content_type\":\"text/plain\",\"content_id\":\"cid-hello\"},"
                 "{\"filename\":\"doc.pdf\",\"path\":\"https://example.com/doc.pdf\"}"
                 "]}") == 0);

    resend_sent_free(&sent);
    resend_free(client);
    fake_stop(f);
}

/* content and path together are rejected with no request. */
static void test_send_attachment_both(void) {
    fake_exchange exchange = {
        .status = 200,
        .body = "{\"id\":\"x\"}",
    };
    fake *f = fake_start(&exchange, 1);
    CHECK(f != NULL);
    if (!f)
        return;

    char base[64];
    snprintf(base, sizeof base, "http://127.0.0.1:%d", fake_port(f));

    static const unsigned char hello[] = "hello";
    const char *const to[] = {"you@example.com"};
    const resend_attachment attachments[] = {
        {
            .filename = "hello.txt",
            .content = hello,
            .content_len = 5,
            .path = "https://example.com/hello.txt",
        },
    };
    resend_send message = {
        .from = "Acme <hello@example.com>",
        .to = to,
        .to_count = 1,
        .subject = "Hello",
        .text = "Hi",
        .attachments = attachments,
        .attachment_count = 1,
    };
    resend_sent sent = {0};

    resend *client = resend_init("re_test");
    CHECK(client != NULL);
    if (!client) {
        fake_stop(f);
        return;
    }

    CHECK(resend_set_base_url(client, base) == 0);
    int rc = resend_emails_send(client, &message, &sent);
    CHECK(rc == -1);
    CHECK(sent.id == NULL);
    CHECK(fake_request_count(f) == 0);
    const resend_error *err = resend_last_error(client);
    CHECK(err != NULL && err->status == 0);
    CHECK(err->message != NULL && strcmp(err->message, "attachment has both content and path") == 0);

    const resend_attachment neither = {.filename = "empty.txt"};
    message.attachments = &neither;
    CHECK(resend_emails_send(client, &message, &sent) == -1);
    err = resend_last_error(client);
    CHECK(err->message != NULL && strcmp(err->message, "attachment needs content or path") == 0);

    /* A count with a NULL array is rejected before the writers index it. */
    message.attachments = NULL;
    message.attachment_count = 0;
    message.to = NULL;
    CHECK(resend_emails_send(client, &message, &sent) == -1);
    err = resend_last_error(client);
    CHECK(err->name != NULL && strcmp(err->name, "missing_required_field") == 0);
    CHECK(err->message != NULL && strcmp(err->message, "missing to") == 0);

    message.to = to;
    message.tag_count = 1;
    CHECK(resend_emails_send(client, &message, &sent) == -1);
    err = resend_last_error(client);
    CHECK(err->message != NULL && strcmp(err->message, "missing tags") == 0);

    message.tag_count = 0;
    message.cc_count = 2;
    const resend_send batch_messages[] = {message};
    resend_batch batch = {.messages = batch_messages, .count = 1};
    resend_sent_list list = {0};
    CHECK(resend_emails_send_batch(client, &batch, &list) == -1);
    err = resend_last_error(client);
    CHECK(err->message != NULL && strcmp(err->message, "missing cc") == 0);
    CHECK(fake_request_count(f) == 0);

    resend_sent_list_free(&list);
    resend_sent_free(&sent);
    resend_free(client);
    fake_stop(f);
}

/* Idempotency-Key is a request header, not a JSON field. */
static void test_send_idempotency_key(void) {
    static const char id[] = "49a3999c-0ce1-4ea6-ab68-afcd6dc2e794";
    char raw[64];
    snprintf(raw, sizeof raw, "{\"id\":\"%s\"}", id);

    fake_exchange exchange = {
        .status = 200,
        .body = raw,
    };
    fake *f = fake_start(&exchange, 1);
    CHECK(f != NULL);
    if (!f)
        return;

    char base[64];
    snprintf(base, sizeof base, "http://127.0.0.1:%d", fake_port(f));

    const char *const to[] = {"you@example.com"};
    resend_send message = smoke_message(to);
    message.idempotency_key = "hello-1";
    resend_sent sent = {0};

    resend *client = resend_init("re_test");
    CHECK(client != NULL);
    if (!client) {
        fake_stop(f);
        return;
    }

    CHECK(resend_set_base_url(client, base) == 0);
    int rc = resend_emails_send(client, &message, &sent);
    CHECK(rc == 0);
    CHECK(sent.id != NULL && strcmp(sent.id, id) == 0);
    CHECK(header_eq(f, "Idempotency-Key", "hello-1"));
    CHECK(fake_body(f) != NULL && strstr(fake_body(f), "idempotency") == NULL);
    CHECK(fake_body(f) != NULL &&
          strcmp(fake_body(f),
                 "{\"from\":\"Acme <hello@example.com>\",\"to\":[\"you@example.com\"],"
                 "\"subject\":\"Hello\",\"text\":\"Hello from C.\"}") == 0);

    resend_sent_free(&sent);
    resend_free(client);
    fake_stop(f);
}

/* Over 256 characters is rejected with no request. 256 is accepted. */
static void test_send_idempotency_key_max(void) {
    static const char id[] = "49a3999c-0ce1-4ea6-ab68-afcd6dc2e794";
    char raw[64];
    snprintf(raw, sizeof raw, "{\"id\":\"%s\"}", id);

    fake_exchange exchange = {
        .status = 200,
        .body = raw,
    };
    fake *f = fake_start(&exchange, 1);
    CHECK(f != NULL);
    if (!f)
        return;

    char base[64];
    snprintf(base, sizeof base, "http://127.0.0.1:%d", fake_port(f));

    char key257[258];
    memset(key257, 'a', 257);
    key257[257] = '\0';

    const char *const to[] = {"you@example.com"};
    resend_send message = smoke_message(to);
    message.idempotency_key = key257;
    resend_sent sent = {0};

    resend *client = resend_init("re_test");
    CHECK(client != NULL);
    if (!client) {
        fake_stop(f);
        return;
    }

    CHECK(resend_set_base_url(client, base) == 0);
    int rc = resend_emails_send(client, &message, &sent);
    CHECK(rc == -1);
    CHECK(sent.id == NULL);
    CHECK(fake_request_count(f) == 0);
    const resend_error *err = resend_last_error(client);
    CHECK(err != NULL && err->status == 0);
    CHECK(err->message != NULL && err->message[0] != '\0');

    char key256[257];
    memset(key256, 'b', 256);
    key256[256] = '\0';
    message.idempotency_key = key256;
    rc = resend_emails_send(client, &message, &sent);
    CHECK(rc == 0);
    CHECK(fake_request_count(f) == 1);
    CHECK(header_eq(f, "Idempotency-Key", key256));

    resend_sent_free(&sent);
    resend_free(client);
    fake_stop(f);
}

/* GET /emails/{id} fills resend_email, including scheduled_at and tags.
 * JSON null scheduled_at stays NULL. */
static void test_emails_get(void) {
    static const char body[] =
        "{\"object\":\"email\","
        "\"id\":\"4ef9a417-02e9-4d39-ad75-9611e0fcc33c\","
        "\"message_id\":\"<111-222-333@email.example.com>\","
        "\"to\":[\"delivered@resend.dev\",\"also@example.com\"],"
        "\"from\":\"Acme <onboarding@resend.dev>\","
        "\"created_at\":\"2026-04-03 22:13:42.674981+00\","
        "\"subject\":\"Hello World\","
        "\"html\":\"Congrats on sending your <strong>first email</strong>!\","
        "\"text\":null,"
        "\"bcc\":[],"
        "\"cc\":[\"cc@example.com\"],"
        "\"reply_to\":[\"reply@example.com\"],"
        "\"last_event\":\"delivered\","
        "\"scheduled_at\":null,"
        "\"tags\":[{\"name\":\"category\",\"value\":\"confirm_email\"}]}";

    fake_exchange exchange = {
        .status = 200,
        .body = body,
    };
    fake *f = fake_start(&exchange, 1);
    CHECK(f != NULL);
    if (!f)
        return;

    char base[64];
    snprintf(base, sizeof base, "http://127.0.0.1:%d", fake_port(f));

    resend_email email = {0};
    resend *client = resend_init("re_test");
    CHECK(client != NULL);
    if (!client) {
        fake_stop(f);
        return;
    }

    CHECK(resend_set_base_url(client, base) == 0);
    int rc = resend_emails_get(client, "4ef9a417-02e9-4d39-ad75-9611e0fcc33c", &email);
    CHECK(rc == 0);
    CHECK(fake_request_count(f) == 1);
    CHECK(fake_method(f) != NULL && strcmp(fake_method(f), "GET") == 0);
    CHECK(fake_path(f) != NULL &&
          strcmp(fake_path(f), "/emails/4ef9a417-02e9-4d39-ad75-9611e0fcc33c") == 0);
    CHECK(header_eq(f, "Authorization", "Bearer re_test"));
    CHECK(email.id != NULL && strcmp(email.id, "4ef9a417-02e9-4d39-ad75-9611e0fcc33c") == 0);
    CHECK(email.message_id != NULL &&
          strcmp(email.message_id, "<111-222-333@email.example.com>") == 0);
    CHECK(email.from != NULL && strcmp(email.from, "Acme <onboarding@resend.dev>") == 0);
    CHECK(email.to_count == 2);
    CHECK(email.to != NULL && email.to[0] != NULL && strcmp(email.to[0], "delivered@resend.dev") == 0);
    CHECK(email.to[1] != NULL && strcmp(email.to[1], "also@example.com") == 0);
    CHECK(email.cc_count == 1);
    CHECK(email.cc != NULL && strcmp(email.cc[0], "cc@example.com") == 0);
    CHECK(email.bcc_count == 0);
    CHECK(email.reply_to_count == 1);
    CHECK(email.reply_to != NULL && strcmp(email.reply_to[0], "reply@example.com") == 0);
    CHECK(email.subject != NULL && strcmp(email.subject, "Hello World") == 0);
    CHECK(email.html != NULL &&
          strcmp(email.html, "Congrats on sending your <strong>first email</strong>!") == 0);
    CHECK(email.text == NULL);
    CHECK(email.created_at != NULL &&
          strcmp(email.created_at, "2026-04-03 22:13:42.674981+00") == 0);
    CHECK(email.last_event != NULL && strcmp(email.last_event, "delivered") == 0);
    CHECK(email.scheduled_at == NULL);
    CHECK(email.tag_count == 1);
    CHECK(email.tags != NULL && email.tags[0].name != NULL &&
          strcmp(email.tags[0].name, "category") == 0);
    CHECK(email.tags != NULL && email.tags[0].value != NULL &&
          strcmp(email.tags[0].value, "confirm_email") == 0);

    resend_email_free(&email);
    CHECK(email.tags == NULL);
    CHECK(email.tag_count == 0);
    CHECK(email.scheduled_at == NULL);
    resend_free(client);
    fake_stop(f);

    static const char scheduled[] =
        "{\"id\":\"49a3999c-0ce1-4ea6-ab68-afcd6dc2e794\","
        "\"last_event\":\"scheduled\","
        "\"scheduled_at\":\"2026-04-04 22:13:42+00\","
        "\"tags\":[{\"name\":\"campaign\",\"value\":\"welcome\"},"
        "{\"name\":\"priority\",\"value\":\"high\"}]}";
    fake_exchange later = {.status = 200, .body = scheduled};
    f = fake_start(&later, 1);
    CHECK(f != NULL);
    if (!f)
        return;
    snprintf(base, sizeof base, "http://127.0.0.1:%d", fake_port(f));
    client = resend_init("re_test");
    CHECK(client != NULL);
    if (!client) {
        fake_stop(f);
        return;
    }
    CHECK(resend_set_base_url(client, base) == 0);
    rc = resend_emails_get(client, "49a3999c-0ce1-4ea6-ab68-afcd6dc2e794", &email);
    CHECK(rc == 0);
    CHECK(email.scheduled_at != NULL &&
          strcmp(email.scheduled_at, "2026-04-04 22:13:42+00") == 0);
    CHECK(email.last_event != NULL && strcmp(email.last_event, "scheduled") == 0);
    CHECK(email.tag_count == 2);
    CHECK(email.tags != NULL && email.tags[0].name != NULL &&
          strcmp(email.tags[0].name, "campaign") == 0);
    CHECK(email.tags != NULL && email.tags[0].value != NULL &&
          strcmp(email.tags[0].value, "welcome") == 0);
    CHECK(email.tags != NULL && email.tags[1].name != NULL &&
          strcmp(email.tags[1].name, "priority") == 0);
    CHECK(email.tags != NULL && email.tags[1].value != NULL &&
          strcmp(email.tags[1].value, "high") == 0);
    resend_email_free(&email);
    resend_free(client);
    fake_stop(f);

    /* Five tags force the array past the initial capacity of four. */
    static const char five_tags[] =
        "{\"id\":\"49a3999c-0ce1-4ea6-ab68-afcd6dc2e794\","
        "\"tags\":["
        "{\"name\":\"a\",\"value\":\"1\"},"
        "{\"name\":\"b\",\"value\":\"2\"},"
        "{\"name\":\"c\",\"value\":\"3\"},"
        "{\"name\":\"d\",\"value\":\"4\"},"
        "{\"name\":\"e\",\"value\":\"5\"}]}";
    fake_exchange five = {.status = 200, .body = five_tags};
    f = fake_start(&five, 1);
    CHECK(f != NULL);
    if (!f)
        return;
    snprintf(base, sizeof base, "http://127.0.0.1:%d", fake_port(f));
    client = resend_init("re_test");
    CHECK(client != NULL);
    if (!client) {
        fake_stop(f);
        return;
    }
    CHECK(resend_set_base_url(client, base) == 0);
    rc = resend_emails_get(client, "49a3999c-0ce1-4ea6-ab68-afcd6dc2e794", &email);
    CHECK(rc == 0);
    CHECK(email.tag_count == 5);
    CHECK(email.tags != NULL && email.tags[0].name != NULL &&
          strcmp(email.tags[0].name, "a") == 0);
    CHECK(email.tags != NULL && email.tags[0].value != NULL &&
          strcmp(email.tags[0].value, "1") == 0);
    CHECK(email.tags != NULL && email.tags[4].name != NULL &&
          strcmp(email.tags[4].name, "e") == 0);
    CHECK(email.tags != NULL && email.tags[4].value != NULL &&
          strcmp(email.tags[4].value, "5") == 0);
    resend_email_free(&email);
    resend_free(client);
    fake_stop(f);

    static const char empty_tags[] =
        "{\"id\":\"49a3999c-0ce1-4ea6-ab68-afcd6dc2e794\","
        "\"scheduled_at\":null,\"tags\":[]}";
    fake_exchange empty = {.status = 200, .body = empty_tags};
    f = fake_start(&empty, 1);
    CHECK(f != NULL);
    if (!f)
        return;
    snprintf(base, sizeof base, "http://127.0.0.1:%d", fake_port(f));
    client = resend_init("re_test");
    CHECK(client != NULL);
    if (!client) {
        fake_stop(f);
        return;
    }
    CHECK(resend_set_base_url(client, base) == 0);
    rc = resend_emails_get(client, "49a3999c-0ce1-4ea6-ab68-afcd6dc2e794", &email);
    CHECK(rc == 0);
    CHECK(email.scheduled_at == NULL);
    CHECK(email.tags == NULL);
    CHECK(email.tag_count == 0);
    resend_email_free(&email);
    resend_free(client);
    fake_stop(f);

    static const char null_tags[] =
        "{\"id\":\"49a3999c-0ce1-4ea6-ab68-afcd6dc2e794\",\"tags\":null}";
    fake_exchange null_ex = {.status = 200, .body = null_tags};
    f = fake_start(&null_ex, 1);
    CHECK(f != NULL);
    if (!f)
        return;
    snprintf(base, sizeof base, "http://127.0.0.1:%d", fake_port(f));
    client = resend_init("re_test");
    CHECK(client != NULL);
    if (!client) {
        fake_stop(f);
        return;
    }
    CHECK(resend_set_base_url(client, base) == 0);
    rc = resend_emails_get(client, "49a3999c-0ce1-4ea6-ab68-afcd6dc2e794", &email);
    CHECK(rc == 0);
    CHECK(email.tags == NULL);
    CHECK(email.tag_count == 0);
    resend_email_free(&email);
    resend_free(client);
    fake_stop(f);
}

/* List uses resend_page; both cursors rejected with no request; limit 0 omits. */
static void test_emails_list(void) {
    static const char body[] =
        "{\"object\":\"list\",\"has_more\":true,\"data\":["
        "{\"id\":\"4ef9a417-02e9-4d39-ad75-9611e0fcc33c\","
        "\"message_id\":\"<111-222-333@email.example.com>\","
        "\"to\":[\"delivered@resend.dev\"],"
        "\"from\":\"Acme <onboarding@resend.dev>\","
        "\"created_at\":\"2026-04-03 22:13:42.674981+00\","
        "\"subject\":\"Hello World\","
        "\"bcc\":null,\"cc\":null,\"reply_to\":null,"
        "\"last_event\":\"delivered\","
        "\"scheduled_at\":\"2026-04-04 22:13:42+00\"},"
        "{\"id\":\"3a9f8c2b-1e5d-4f8a-9c7b-2d6e5f8a9c7b\","
        "\"message_id\":\"<444-555-666@email.example.com>\","
        "\"to\":[\"user@example.com\"],"
        "\"from\":\"Acme <onboarding@resend.dev>\","
        "\"created_at\":\"2026-04-03 21:45:12.345678+00\","
        "\"subject\":\"Welcome to Acme\","
        "\"bcc\":null,\"cc\":null,\"reply_to\":null,"
        "\"last_event\":\"opened\","
        "\"scheduled_at\":null}]}";

    fake_exchange exchange = {
        .status = 200,
        .body = body,
    };
    fake *f = fake_start(&exchange, 1);
    CHECK(f != NULL);
    if (!f)
        return;

    char base[64];
    snprintf(base, sizeof base, "http://127.0.0.1:%d", fake_port(f));

    resend *client = resend_init("re_test");
    CHECK(client != NULL);
    if (!client) {
        fake_stop(f);
        return;
    }
    CHECK(resend_set_base_url(client, base) == 0);
    resend_page both = {.after = "a", .before = "b", .limit = 10};
    resend_email_list list = {0};
    int rc = resend_emails_list(client, &both, &list);
    CHECK(rc == -1);
    CHECK(fake_request_count(f) == 0);
    const resend_error *err = resend_last_error(client);
    CHECK(err != NULL && err->status == 0);
    CHECK(err->message != NULL && err->message[0] != '\0');

    resend_page page = {.after = "cursor-1", .limit = 0};
    rc = resend_emails_list(client, &page, &list);
    CHECK(rc == 0);
    CHECK(fake_request_count(f) == 1);
    CHECK(fake_method(f) != NULL && strcmp(fake_method(f), "GET") == 0);
    CHECK(fake_path(f) != NULL && strcmp(fake_path(f), "/emails?after=cursor-1") == 0);
    CHECK(list.has_more == 1);
    CHECK(list.count == 2);
    CHECK(list.items != NULL);
    CHECK(list.items[0].id != NULL &&
          strcmp(list.items[0].id, "4ef9a417-02e9-4d39-ad75-9611e0fcc33c") == 0);
    CHECK(list.items[0].message_id != NULL &&
          strcmp(list.items[0].message_id, "<111-222-333@email.example.com>") == 0);
    CHECK(list.items[0].last_event != NULL && strcmp(list.items[0].last_event, "delivered") == 0);
    CHECK(list.items[0].html == NULL);
    CHECK(list.items[0].text == NULL);
    CHECK(list.items[0].bcc_count == 0);
    CHECK(list.items[0].scheduled_at != NULL &&
          strcmp(list.items[0].scheduled_at, "2026-04-04 22:13:42+00") == 0);
    CHECK(list.items[0].tags == NULL);
    CHECK(list.items[0].tag_count == 0);
    CHECK(list.items[1].id != NULL &&
          strcmp(list.items[1].id, "3a9f8c2b-1e5d-4f8a-9c7b-2d6e5f8a9c7b") == 0);
    CHECK(list.items[1].last_event != NULL && strcmp(list.items[1].last_event, "opened") == 0);
    CHECK(list.items[1].scheduled_at == NULL);
    CHECK(list.items[1].tags == NULL);
    CHECK(list.items[1].tag_count == 0);

    resend_email_list_free(&list);
    resend_free(client);
    fake_stop(f);

    /* Fresh fake: accessors keep only the first request. */
    f = fake_start(&exchange, 1);
    CHECK(f != NULL);
    if (!f)
        return;
    snprintf(base, sizeof base, "http://127.0.0.1:%d", fake_port(f));
    client = resend_init("re_test");
    CHECK(client != NULL);
    if (!client) {
        fake_stop(f);
        return;
    }
    CHECK(resend_set_base_url(client, base) == 0);
    resend_page limited = {.before = "cursor-2", .limit = 5};
    memset(&list, 0, sizeof list);
    rc = resend_emails_list(client, &limited, &list);
    CHECK(rc == 0);
    CHECK(fake_path(f) != NULL && strcmp(fake_path(f), "/emails?before=cursor-2&limit=5") == 0);

    resend_email_list_free(&list);
    resend_free(client);
    fake_stop(f);
}

/* POST /emails/{id}/cancel returns the email (last_event canceled). */
static void test_emails_cancel(void) {
    static const char body[] =
        "{\"object\":\"email\","
        "\"id\":\"49a3999c-0ce1-4ea6-ab68-afcd6dc2e794\","
        "\"message_id\":\"<sched@email.example.com>\","
        "\"to\":[\"you@example.com\"],"
        "\"from\":\"Acme <hello@example.com>\","
        "\"created_at\":\"2026-04-03 22:13:42.674981+00\","
        "\"subject\":\"Hello\","
        "\"html\":null,"
        "\"text\":\"Hello from C.\","
        "\"bcc\":[],\"cc\":[],\"reply_to\":[],"
        "\"last_event\":\"canceled\"}";

    fake_exchange exchange = {
        .status = 200,
        .body = body,
    };
    fake *f = fake_start(&exchange, 1);
    CHECK(f != NULL);
    if (!f)
        return;

    char base[64];
    snprintf(base, sizeof base, "http://127.0.0.1:%d", fake_port(f));

    resend_email email = {0};
    resend *client = resend_init("re_test");
    CHECK(client != NULL);
    if (!client) {
        fake_stop(f);
        return;
    }

    CHECK(resend_set_base_url(client, base) == 0);
    int rc = resend_emails_cancel(client, "49a3999c-0ce1-4ea6-ab68-afcd6dc2e794", &email);
    CHECK(rc == 0);
    CHECK(fake_request_count(f) == 1);
    CHECK(fake_method(f) != NULL && strcmp(fake_method(f), "POST") == 0);
    CHECK(fake_path(f) != NULL &&
          strcmp(fake_path(f), "/emails/49a3999c-0ce1-4ea6-ab68-afcd6dc2e794/cancel") == 0);
    CHECK(email.id != NULL && strcmp(email.id, "49a3999c-0ce1-4ea6-ab68-afcd6dc2e794") == 0);
    CHECK(email.last_event != NULL && strcmp(email.last_event, "canceled") == 0);
    CHECK(email.message_id != NULL && strcmp(email.message_id, "<sched@email.example.com>") == 0);
    CHECK(email.subject != NULL && strcmp(email.subject, "Hello") == 0);

    resend_email_free(&email);
    resend_free(client);
    fake_stop(f);
}

/* Batch: count > 100 rejected with no request; only batch idempotency key is sent. */
static void test_emails_send_batch(void) {
    static const char body[] =
        "{\"data\":[{\"id\":\"49a3999c-0ce1-4ea6-ab68-afcd6dc2e794\"},"
        "{\"id\":\"3a9f8c2b-1e5d-4f8a-9c7b-2d6e5f8a9c7b\"}]}";

    fake_exchange exchange = {
        .status = 200,
        .body = body,
    };
    fake *f = fake_start(&exchange, 1);
    CHECK(f != NULL);
    if (!f)
        return;

    char base[64];
    snprintf(base, sizeof base, "http://127.0.0.1:%d", fake_port(f));

    const char *const to1[] = {"one@example.com"};
    const char *const to2[] = {"two@example.com"};
    resend_send messages[2] = {
        {
            .from = "Acme <hello@example.com>",
            .to = to1,
            .to_count = 1,
            .subject = "One",
            .text = "first",
            .scheduled_at = "in 1 min",
            .idempotency_key = "msg-key-ignored",
        },
        {
            .from = "Acme <hello@example.com>",
            .to = to2,
            .to_count = 1,
            .subject = "Two",
            .text = "second",
            .idempotency_key = "also-ignored",
        },
    };
    resend_batch batch = {
        .messages = messages,
        .count = 2,
        .idempotency_key = "batch-1",
    };
    resend_sent_list sent = {0};

    resend *client = resend_init("re_test");
    CHECK(client != NULL);
    if (!client) {
        fake_stop(f);
        return;
    }
    CHECK(resend_set_base_url(client, base) == 0);
    int rc = resend_emails_send_batch(client, &batch, &sent);
    CHECK(rc == 0);
    CHECK(fake_request_count(f) == 1);
    CHECK(fake_method(f) != NULL && strcmp(fake_method(f), "POST") == 0);
    CHECK(fake_path(f) != NULL && strcmp(fake_path(f), "/emails/batch") == 0);
    CHECK(header_eq(f, "Idempotency-Key", "batch-1"));
    CHECK(fake_header(f, "x-batch-validation") == NULL);
    CHECK(fake_body(f) != NULL &&
          strcmp(fake_body(f),
                 "[{\"from\":\"Acme <hello@example.com>\",\"to\":[\"one@example.com\"],"
                 "\"subject\":\"One\",\"text\":\"first\",\"scheduled_at\":\"in 1 min\"},"
                 "{\"from\":\"Acme <hello@example.com>\",\"to\":[\"two@example.com\"],"
                 "\"subject\":\"Two\",\"text\":\"second\"}]") == 0);
    CHECK(fake_body(f) != NULL && strstr(fake_body(f), "idempotency") == NULL);
    CHECK(sent.count == 2);
    CHECK(sent.items != NULL);
    CHECK(sent.items[0].id != NULL &&
          strcmp(sent.items[0].id, "49a3999c-0ce1-4ea6-ab68-afcd6dc2e794") == 0);
    CHECK(sent.items[1].id != NULL &&
          strcmp(sent.items[1].id, "3a9f8c2b-1e5d-4f8a-9c7b-2d6e5f8a9c7b") == 0);

    resend_sent_list_free(&sent);
    resend_free(client);
    fake_stop(f);

    /* Over 100 is rejected with no request. */
    f = fake_start(&exchange, 1);
    CHECK(f != NULL);
    if (!f)
        return;
    snprintf(base, sizeof base, "http://127.0.0.1:%d", fake_port(f));
    client = resend_init("re_test");
    CHECK(client != NULL);
    if (!client) {
        fake_stop(f);
        return;
    }
    CHECK(resend_set_base_url(client, base) == 0);
    const char *const to[] = {"you@example.com"};
    resend_batch missing = {.count = 1};
    memset(&sent, 0, sizeof sent);
    rc = resend_emails_send_batch(client, &missing, &sent);
    CHECK(rc == -1);
    CHECK(fake_request_count(f) == 0);
    CHECK(sent.items == NULL);

    resend_send bare = smoke_message(to);
    bare.attachment_count = 1;
    resend_batch bare_batch = {.messages = &bare, .count = 1};
    rc = resend_emails_send_batch(client, &bare_batch, &sent);
    CHECK(rc == -1);
    CHECK(fake_request_count(f) == 0);

    static const unsigned char byte = 'x';
    resend_attachment both = {
        .filename = "a.txt",
        .content = &byte,
        .content_len = 1,
        .path = "https://example.com/a.txt",
    };
    resend_send bad_msg = smoke_message(to);
    bad_msg.attachments = &both;
    bad_msg.attachment_count = 1;
    resend_batch bad_batch = {.messages = &bad_msg, .count = 1};
    rc = resend_emails_send_batch(client, &bad_batch, &sent);
    CHECK(rc == -1);
    CHECK(fake_request_count(f) == 0);

    char long_key[258];
    memset(long_key, 'k', sizeof long_key - 1);
    long_key[sizeof long_key - 1] = '\0';
    resend_send keyed = smoke_message(to);
    resend_batch long_batch = {.messages = &keyed, .count = 1, .idempotency_key = long_key};
    rc = resend_emails_send_batch(client, &long_batch, &sent);
    CHECK(rc == -1);
    CHECK(fake_request_count(f) == 0);

    resend_send injected = smoke_message(to);
    resend_batch inj = {.messages = &injected, .count = 1, .idempotency_key = "bad\r\nX-Injected: yes"};
    rc = resend_emails_send_batch(client, &inj, &sent);
    CHECK(rc == -1);
    CHECK(fake_request_count(f) == 0);

    resend *bad_key = resend_init("re_test\r\nX-Injected: yes");
    CHECK(bad_key != NULL);
    if (bad_key) {
        CHECK(resend_set_base_url(bad_key, base) == 0);
        resend_batch one = {.messages = &injected, .count = 1};
        rc = resend_emails_send_batch(bad_key, &one, &sent);
        CHECK(rc == -1);
        CHECK(fake_request_count(f) == 0);
        resend_free(bad_key);
    }

    CHECK(resend_set_base_url(client, "http://127.0.0.1:1\r\nX: y") == 0);
    resend_batch plain = {.messages = &injected, .count = 1};
    rc = resend_emails_send_batch(client, &plain, &sent);
    CHECK(rc == -1);
    CHECK(fake_request_count(f) == 0);
    CHECK(resend_set_base_url(client, base) == 0);
    resend_send too_many[101];
    for (size_t i = 0; i < 101; i++)
        too_many[i] = smoke_message(to);
    resend_batch over = {.messages = too_many, .count = 101};
    memset(&sent, 0, sizeof sent);
    rc = resend_emails_send_batch(client, &over, &sent);
    CHECK(rc == -1);
    CHECK(fake_request_count(f) == 0);
    const resend_error *err = resend_last_error(client);
    CHECK(err != NULL && err->status == 0);
    CHECK(err->message != NULL && err->message[0] != '\0');
    CHECK(sent.items == NULL);
    CHECK(sent.count == 0);

    resend_free(client);
    fake_stop(f);
}

static resend *client_on(fake *f);

/* PATCH /emails/{id} sends scheduled_at only and fills the id. */
static void test_emails_update(void) {
    static const char id[] = "49a3999c-0ce1-4ea6-ab68-afcd6dc2e794";
    static const char body[] =
        "{\"object\":\"email\",\"id\":\"49a3999c-0ce1-4ea6-ab68-afcd6dc2e794\","
        "\"scheduled_at\":\"in 1 min\",\"last_event\":\"scheduled\"}";
    fake_exchange exchange = {.status = 200, .body = body};
    fake *f = fake_start(&exchange, 1);
    CHECK(f != NULL);
    if (!f)
        return;
    resend *client = client_on(f);
    CHECK(client != NULL);
    if (!client) {
        fake_stop(f);
        return;
    }

    resend_sent sent = {0};
    int rc = resend_emails_update(client, id, "in 1 min", &sent);
    CHECK(rc == 0);
    CHECK(fake_request_count(f) == 1);
    CHECK(fake_method(f) != NULL && strcmp(fake_method(f), "PATCH") == 0);
    CHECK(fake_path(f) != NULL &&
          strcmp(fake_path(f), "/emails/49a3999c-0ce1-4ea6-ab68-afcd6dc2e794") == 0);
    CHECK(body_eq(f, "{\"scheduled_at\":\"in 1 min\"}"));
    CHECK(header_eq(f, "Authorization", "Bearer re_test"));
    CHECK(header_eq(f, "Content-Type", "application/json"));
    CHECK(fake_header(f, "Idempotency-Key") == NULL);
    CHECK(sent.id != NULL && strcmp(sent.id, id) == 0);
    resend_sent_free(&sent);
    resend_free(client);
    fake_stop(f);

    f = fake_start(&exchange, 1);
    CHECK(f != NULL);
    if (!f)
        return;
    client = client_on(f);
    CHECK(client != NULL);
    if (!client) {
        fake_stop(f);
        return;
    }
    memset(&sent, 0, sizeof sent);
    rc = resend_emails_update(client, id, "in \"1\" min", &sent);
    CHECK(rc == 0);
    CHECK(body_eq(f, "{\"scheduled_at\":\"in \\\"1\\\" min\"}"));
    CHECK(sent.id != NULL && strcmp(sent.id, id) == 0);
    resend_sent_free(&sent);
    resend_free(client);
    fake_stop(f);

    f = fake_start(&exchange, 1);
    CHECK(f != NULL);
    if (!f)
        return;
    client = client_on(f);
    CHECK(client != NULL);
    if (!client) {
        fake_stop(f);
        return;
    }
    memset(&sent, 0x5a, sizeof sent);
    rc = resend_emails_update(client, id, NULL, &sent);
    CHECK(rc == -1);
    CHECK(sent.id == NULL);
    rc = resend_emails_update(client, id, "", &sent);
    CHECK(rc == -1);
    CHECK(sent.id == NULL);
    const resend_error *err = resend_last_error(client);
    CHECK(err != NULL && err->status == 0);
    CHECK(err->message != NULL && strcmp(err->message, "missing scheduled_at") == 0);
    CHECK(resend_emails_update(client, "", "in 1 min", &sent) == -1);
    CHECK(resend_emails_update(client, "..", "in 1 min", &sent) == -1);
    CHECK(resend_emails_update(client, "%2e%2e", "in 1 min", &sent) == -1);
    CHECK(resend_emails_update(NULL, id, "in 1 min", &sent) == -1);
    CHECK(fake_request_count(f) == 0);
    resend_free(client);
    fake_stop(f);

    fake_exchange missing = {
        .status = 422,
        .body = "{\"name\":\"validation_error\",\"message\":\"scheduled_at is invalid\"}",
    };
    f = fake_start(&missing, 1);
    CHECK(f != NULL);
    if (!f)
        return;
    client = client_on(f);
    CHECK(client != NULL);
    if (!client) {
        fake_stop(f);
        return;
    }
    memset(&sent, 0, sizeof sent);
    rc = resend_emails_update(client, id, "yesterday", &sent);
    CHECK(rc == -1);
    CHECK(sent.id == NULL);
    err = resend_last_error(client);
    CHECK(err != NULL && err->status == 422);
    CHECK(err->name != NULL && strcmp(err->name, "validation_error") == 0);
    CHECK(err->message != NULL && strcmp(err->message, "scheduled_at is invalid") == 0);
    resend_sent_free(&sent);
    resend_free(client);
    fake_stop(f);

    fake_exchange no_id = {.status = 200, .body = "{\"object\":\"email\"}"};
    f = fake_start(&no_id, 1);
    CHECK(f != NULL);
    if (!f)
        return;
    client = client_on(f);
    CHECK(client != NULL);
    if (!client) {
        fake_stop(f);
        return;
    }
    memset(&sent, 0, sizeof sent);
    rc = resend_emails_update(client, id, "in 1 min", &sent);
    CHECK(rc == -1);
    CHECK(sent.id == NULL);
    err = resend_last_error(client);
    CHECK(err != NULL && err->status == 200);
    CHECK(err->message != NULL && strcmp(err->message, "missing id") == 0);
    resend_free(client);
    fake_stop(f);

    f = fake_start(&exchange, 1);
    CHECK(f != NULL);
    if (!f)
        return;
    client = client_on(f);
    CHECK(client != NULL);
    if (!client) {
        fake_stop(f);
        return;
    }
    memset(&sent, 0, sizeof sent);
    rc = resend_emails_update(client, "a/b c\n?", "in 1 min", &sent);
    CHECK(rc == 0);
    CHECK(fake_path(f) != NULL && strcmp(fake_path(f), "/emails/a%2Fb%20c%0A%3F") == 0);
    resend_sent_free(&sent);
    resend_free(client);
    fake_stop(f);
}

/* Several errors grow the array; null errors is none; a malformed errors fails and frees. */
static void test_emails_batch_errors(void) {
    static const char many[] =
        "{\"data\":[{\"id\":\"a\"}],\"errors\":["
        "{\"index\":1,\"message\":\"e1\"},{\"index\":2,\"message\":\"e2\"},"
        "{\"index\":3,\"message\":\"e3\"},{\"index\":4,\"message\":\"e4\"},"
        "{\"index\":5,\"message\":\"e5\"}]}";
    static const char null_errors[] = "{\"data\":[{\"id\":\"a\"}],\"errors\":null}";
    static const char not_array[] = "{\"data\":[{\"id\":\"a\"}],\"errors\":{}}";
    static const char null_index[] =
        "{\"data\":[{\"id\":\"a\"}],\"errors\":[{\"index\":null,\"message\":\"e\"}]}";
    static const char no_message[] = "{\"data\":[{\"id\":\"a\"}],\"errors\":[{\"index\":0}]}";
    fake_exchange exchanges[] = {
        {.status = 200, .body = many},
        {.status = 200, .body = null_errors},
        {.status = 200, .body = not_array},
        {.status = 200, .body = null_index},
        {.status = 200, .body = no_message},
    };
    fake *f = fake_start(exchanges, 5);
    CHECK(f != NULL);
    if (!f)
        return;
    resend *client = client_on(f);
    CHECK(client != NULL);
    if (!client) {
        fake_stop(f);
        return;
    }
    const char *const to[] = {"one@example.com"};
    resend_send message = smoke_message(to);
    resend_batch batch = {.messages = &message, .count = 1, .validation = "permissive"};
    resend_sent_list sent = {0};

    CHECK(resend_emails_send_batch(client, &batch, &sent) == 0);
    CHECK(sent.count == 1 && sent.error_count == 5);
    for (size_t i = 0; sent.errors && i < sent.error_count; i++) {
        char want[32];
        snprintf(want, sizeof want, "e%zu", i + 1);
        CHECK(sent.errors[i].index == i + 1 && sent.errors[i].message != NULL &&
              strcmp(sent.errors[i].message, want) == 0);
    }
    resend_sent_list_free(&sent);

    CHECK(resend_emails_send_batch(client, &batch, &sent) == 0);
    CHECK(sent.count == 1 && sent.errors == NULL && sent.error_count == 0);
    resend_sent_list_free(&sent);

    for (int i = 0; i < 3; i++) {
        CHECK(resend_emails_send_batch(client, &batch, &sent) == -1);
        CHECK(sent.items == NULL && sent.count == 0);
        CHECK(sent.errors == NULL && sent.error_count == 0);
    }
    CHECK(fake_request_count(f) == 5);
    resend_free(client);
    fake_stop(f);
}

/*
 * validation is strict or permissive, sent as x-batch-validation.
 * NULL omits the header. Any other value is rejected with no request.
 * A permissive 2xx may include errors; those are returned with the ids.
 */
static void test_emails_batch_validation(void) {
    static const char permissive_body[] =
        "{\"data\":[{\"id\":\"49a3999c-0ce1-4ea6-ab68-afcd6dc2e794\"}],"
        "\"errors\":[{\"index\":1,\"message\":\"Invalid email\"}]}";
    fake_exchange permissive = {.status = 200, .body = permissive_body};
    fake *f = fake_start(&permissive, 1);
    CHECK(f != NULL);
    if (!f)
        return;
    resend *client = client_on(f);
    CHECK(client != NULL);
    if (!client) {
        fake_stop(f);
        return;
    }

    const char *const to[] = {"one@example.com"};
    resend_send message = smoke_message(to);
    message.idempotency_key = "msg-key-ignored";
    resend_batch batch = {
        .messages = &message,
        .count = 1,
        .idempotency_key = "batch-1",
        .validation = "permissive",
    };
    resend_sent_list sent = {0};
    int rc = resend_emails_send_batch(client, &batch, &sent);
    CHECK(rc == 0);
    CHECK(fake_request_count(f) == 1);
    CHECK(header_eq(f, "x-batch-validation", "permissive"));
    CHECK(header_eq(f, "Idempotency-Key", "batch-1"));
    CHECK(body_eq(f, "[{\"from\":\"Acme <hello@example.com>\",\"to\":[\"one@example.com\"],"
                      "\"subject\":\"Hello\",\"text\":\"Hello from C.\"}]"));
    CHECK(fake_body(f) != NULL && strstr(fake_body(f), "validation") == NULL);
    CHECK(sent.count == 1);
    CHECK(sent.items != NULL && sent.items[0].id != NULL &&
          strcmp(sent.items[0].id, "49a3999c-0ce1-4ea6-ab68-afcd6dc2e794") == 0);
    CHECK(sent.error_count == 1);
    CHECK(sent.errors != NULL && sent.errors[0].index == 1 && sent.errors[0].message != NULL &&
          strcmp(sent.errors[0].message, "Invalid email") == 0);
    resend_sent_list_free(&sent);
    resend_free(client);
    fake_stop(f);

    static const char empty_data_body[] =
        "{\"data\":[],\"errors\":[{\"index\":0,\"message\":\"Invalid email\"}]}";
    fake_exchange empty_data = {.status = 200, .body = empty_data_body};
    f = fake_start(&empty_data, 1);
    CHECK(f != NULL);
    if (!f)
        return;
    client = client_on(f);
    CHECK(client != NULL);
    if (!client) {
        fake_stop(f);
        return;
    }
    memset(&sent, 0, sizeof sent);
    rc = resend_emails_send_batch(client, &batch, &sent);
    CHECK(rc == 0);
    CHECK(sent.count == 0);
    CHECK(sent.items == NULL);
    CHECK(sent.error_count == 1);
    CHECK(sent.errors != NULL && sent.errors[0].index == 0 && sent.errors[0].message != NULL &&
          strcmp(sent.errors[0].message, "Invalid email") == 0);
    resend_sent_list_free(&sent);
    resend_free(client);
    fake_stop(f);

    static const char strict_body[] =
        "{\"data\":[{\"id\":\"49a3999c-0ce1-4ea6-ab68-afcd6dc2e794\"}]}";
    fake_exchange strict = {.status = 200, .body = strict_body};
    f = fake_start(&strict, 1);
    CHECK(f != NULL);
    if (!f)
        return;
    client = client_on(f);
    CHECK(client != NULL);
    if (!client) {
        fake_stop(f);
        return;
    }
    batch.idempotency_key = NULL;
    batch.validation = "strict";
    memset(&sent, 0, sizeof sent);
    rc = resend_emails_send_batch(client, &batch, &sent);
    CHECK(rc == 0);
    CHECK(header_eq(f, "x-batch-validation", "strict"));
    CHECK(fake_header(f, "Idempotency-Key") == NULL);
    CHECK(sent.count == 1);
    CHECK(sent.errors == NULL);
    CHECK(sent.error_count == 0);
    resend_sent_list_free(&sent);

    const char *bad_modes[] = {"", "Strict", "loose", "permissive\r\nX-Injected: yes"};
    for (size_t i = 0; i < sizeof bad_modes / sizeof bad_modes[0]; i++) {
        batch.validation = bad_modes[i];
        memset(&sent, 0x5a, sizeof sent);
        rc = resend_emails_send_batch(client, &batch, &sent);
        CHECK(rc == -1);
        CHECK(sent.items == NULL);
        CHECK(sent.count == 0);
        CHECK(sent.errors == NULL);
        CHECK(sent.error_count == 0);
        const resend_error *err = resend_last_error(client);
        CHECK(err != NULL && err->status == 0);
        CHECK(err->message != NULL &&
              strcmp(err->message, "validation must be strict or permissive") == 0);
    }
    CHECK(fake_request_count(f) == 1);
    resend_free(client);
    fake_stop(f);
}

/*
 * Sent-email attachments use the received-attachment type. List and get set
 * download_url and expires_at. The path is /emails/{id}/attachments.
 */
static void test_emails_attachments(void) {
    static const char email_id[] = "4ef9a417-02e9-4d39-ad75-9611e0fcc33c";
    static const char att_id[] = "2a0c9ce0-3112-4728-976e-47ddcd16a318";
    static const char list_body[] =
        "{\"object\":\"list\",\"has_more\":true,\"data\":["
        "{\"id\":\"2a0c9ce0-3112-4728-976e-47ddcd16a318\","
        "\"filename\":\"avatar.png\",\"content_type\":\"image/png\","
        "\"content_disposition\":\"inline\",\"content_id\":\"img001\",\"size\":4096,"
        "\"download_url\":\"https://cdn.resend.com/avatar.png?sig=1\","
        "\"expires_at\":\"2026-10-17T14:29:41.521Z\"},"
        "{\"id\":\"3b1d0df1-4223-5839-087f-54eedd27b419\","
        "\"filename\":null,\"content_type\":\"application/pdf\","
        "\"content_disposition\":null,\"content_id\":null,\"size\":13264,"
        "\"download_url\":\"https://cdn.resend.com/doc.pdf?sig=2\","
        "\"expires_at\":\"2026-10-17T14:29:41.521Z\"}"
        "]}";
    static const char get_body[] =
        "{\"object\":\"attachment\","
        "\"id\":\"2a0c9ce0-3112-4728-976e-47ddcd16a318\","
        "\"filename\":\"avatar.png\",\"content_type\":\"image/png\","
        "\"content_disposition\":\"inline\",\"content_id\":\"img001\",\"size\":4096,"
        "\"download_url\":\"https://cdn.resend.com/avatar.png?sig=1\","
        "\"expires_at\":\"2026-10-17T14:29:41.521Z\"}";

    fake_exchange list_ex = {.status = 200, .body = list_body};
    fake *f = fake_start(&list_ex, 1);
    CHECK(f != NULL);
    if (!f)
        return;
    resend *client = client_on(f);
    CHECK(client != NULL);
    if (!client) {
        fake_stop(f);
        return;
    }

    resend_page both = {.after = "a", .before = "b"};
    resend_received_attachment_list list = {0};
    int rc = resend_emails_attachments_list(client, email_id, &both, &list);
    CHECK(rc == -1);
    CHECK(fake_request_count(f) == 0);
    const resend_error *err = resend_last_error(client);
    CHECK(err != NULL && err->status == 0);
    CHECK(err->message != NULL && strcmp(err->message, "after and before cannot be combined") == 0);

    rc = resend_emails_attachments_list(client, "", NULL, &list);
    CHECK(rc == -1);
    CHECK(resend_emails_attachments_list(client, "..", NULL, &list) == -1);
    CHECK(fake_request_count(f) == 0);

    resend_page page = {.after = "cursor-1", .limit = 0};
    rc = resend_emails_attachments_list(client, email_id, &page, &list);
    CHECK(rc == 0);
    CHECK(fake_request_count(f) == 1);
    CHECK(fake_method(f) != NULL && strcmp(fake_method(f), "GET") == 0);
    CHECK(fake_path(f) != NULL &&
          strcmp(fake_path(f),
                 "/emails/4ef9a417-02e9-4d39-ad75-9611e0fcc33c/attachments?after=cursor-1") == 0);
    CHECK(list.has_more == 1);
    CHECK(list.count == 2);
    CHECK(list.items != NULL);
    CHECK(list.items[0].id != NULL && strcmp(list.items[0].id, att_id) == 0);
    CHECK(list.items[0].filename != NULL && strcmp(list.items[0].filename, "avatar.png") == 0);
    CHECK(list.items[0].content_type != NULL &&
          strcmp(list.items[0].content_type, "image/png") == 0);
    CHECK(list.items[0].content_disposition != NULL &&
          strcmp(list.items[0].content_disposition, "inline") == 0);
    CHECK(list.items[0].content_id != NULL && strcmp(list.items[0].content_id, "img001") == 0);
    CHECK(list.items[0].size == 4096);
    CHECK(list.items[0].download_url != NULL &&
          strcmp(list.items[0].download_url, "https://cdn.resend.com/avatar.png?sig=1") == 0);
    CHECK(list.items[0].expires_at != NULL &&
          strcmp(list.items[0].expires_at, "2026-10-17T14:29:41.521Z") == 0);
    CHECK(list.items[1].filename == NULL);
    CHECK(list.items[1].content_disposition == NULL);
    CHECK(list.items[1].content_id == NULL);
    CHECK(list.items[1].size == 13264);
    CHECK(list.items[1].download_url != NULL &&
          strcmp(list.items[1].download_url, "https://cdn.resend.com/doc.pdf?sig=2") == 0);
    resend_received_attachment_list_free(&list);
    resend_free(client);
    fake_stop(f);

    f = fake_start(&list_ex, 1);
    CHECK(f != NULL);
    if (!f)
        return;
    client = client_on(f);
    CHECK(client != NULL);
    if (!client) {
        fake_stop(f);
        return;
    }
    resend_page limited = {.before = "cursor-2", .limit = 5};
    memset(&list, 0, sizeof list);
    rc = resend_emails_attachments_list(client, "a/b", &limited, &list);
    CHECK(rc == 0);
    CHECK(fake_path(f) != NULL &&
          strcmp(fake_path(f), "/emails/a%2Fb/attachments?before=cursor-2&limit=5") == 0);
    resend_received_attachment_list_free(&list);
    resend_free(client);
    fake_stop(f);

    fake_exchange get_ex = {.status = 200, .body = get_body};
    f = fake_start(&get_ex, 1);
    CHECK(f != NULL);
    if (!f)
        return;
    client = client_on(f);
    CHECK(client != NULL);
    if (!client) {
        fake_stop(f);
        return;
    }
    resend_received_attachment att = {0};
    rc = resend_emails_attachment_get(client, email_id, "", &att);
    CHECK(rc == -1);
    rc = resend_emails_attachment_get(client, "", att_id, &att);
    CHECK(rc == -1);
    rc = resend_emails_attachment_get(client, email_id, "..", &att);
    CHECK(rc == -1);
    CHECK(fake_request_count(f) == 0);

    rc = resend_emails_attachment_get(client, email_id, att_id, &att);
    CHECK(rc == 0);
    CHECK(fake_path(f) != NULL &&
          strcmp(fake_path(f),
                 "/emails/4ef9a417-02e9-4d39-ad75-9611e0fcc33c/attachments/"
                 "2a0c9ce0-3112-4728-976e-47ddcd16a318") == 0);
    CHECK(att.id != NULL && strcmp(att.id, att_id) == 0);
    CHECK(att.filename != NULL && strcmp(att.filename, "avatar.png") == 0);
    CHECK(att.content_type != NULL && strcmp(att.content_type, "image/png") == 0);
    CHECK(att.content_disposition != NULL && strcmp(att.content_disposition, "inline") == 0);
    CHECK(att.content_id != NULL && strcmp(att.content_id, "img001") == 0);
    CHECK(att.size == 4096);
    CHECK(att.download_url != NULL &&
          strcmp(att.download_url, "https://cdn.resend.com/avatar.png?sig=1") == 0);
    CHECK(att.expires_at != NULL && strcmp(att.expires_at, "2026-10-17T14:29:41.521Z") == 0);
    resend_received_attachment_free(&att);
    resend_free(client);
    fake_stop(f);

    static const fake_exchange bad[] = {
        {.status = 200, .body = "not-json"},
        {.status = 404, .body = "{\"name\":\"not_found\",\"message\":\"Attachment not found\"}"},
    };
    f = fake_start(bad, sizeof bad / sizeof bad[0]);
    CHECK(f != NULL);
    if (!f)
        return;
    client = client_on(f);
    CHECK(client != NULL);
    if (!client) {
        fake_stop(f);
        return;
    }
    resend_received_attachment_list bad_list = {0};
    rc = resend_emails_attachments_list(client, email_id, NULL, &bad_list);
    CHECK(rc == -1);
    CHECK(bad_list.items == NULL);
    err = resend_last_error(client);
    CHECK(err != NULL && err->status == 200);
    resend_received_attachment_list_free(&bad_list);

    memset(&att, 0, sizeof att);
    rc = resend_emails_attachment_get(client, email_id, att_id, &att);
    CHECK(rc == -1);
    CHECK(att.id == NULL);
    err = resend_last_error(client);
    CHECK(err != NULL && err->status == 404);
    CHECK(err->name != NULL && strcmp(err->name, "not_found") == 0);
    resend_received_attachment_free(&att);
    CHECK(fake_request_count(f) == 2);
    resend_free(client);
    fake_stop(f);
}

static resend *client_on(fake *f) {
    char base[64];
    snprintf(base, sizeof base, "http://127.0.0.1:%d", fake_port(f));
    resend *client = resend_init("re_test");
    if (client)
        CHECK(resend_set_base_url(client, base) == 0);
    return client;
}

/* Id is one path segment. Slash, space, newline, and '?' stay in the segment.
 * `.` and `..` are refused, including %2e. Subject escapes cover a BMP character
 * and an emoji pair. */
static void test_emails_id_path(void) {
    static const char body[] =
        "{\"id\":\"a/b c\\n?\","
        "\"subject\":\"caf\\u00e9 \\uD83D\\uDE00\","
        "\"text\":\"caf\xc3\xa9\"}";
    fake_exchange exchange = {.status = 200, .body = body};
    fake *f = fake_start(&exchange, 1);
    CHECK(f != NULL);
    if (!f)
        return;
    resend *client = client_on(f);
    CHECK(client != NULL);
    if (!client) {
        fake_stop(f);
        return;
    }

    const char *id = "a/b c\n?";
    resend_email email = {0};
    int rc = resend_emails_get(client, id, &email);
    CHECK(rc == 0);
    CHECK(fake_path(f) != NULL && strcmp(fake_path(f), "/emails/a%2Fb%20c%0A%3F") == 0);
    CHECK(email.id != NULL && strcmp(email.id, id) == 0);
    CHECK(email.subject != NULL && strcmp(email.subject, "caf\xc3\xa9 \xf0\x9f\x98\x80") == 0);
    CHECK(email.text != NULL && strcmp(email.text, "caf\xc3\xa9") == 0);
    resend_email_free(&email);
    resend_free(client);
    fake_stop(f);

    f = fake_start(&exchange, 1);
    CHECK(f != NULL);
    if (!f)
        return;
    client = client_on(f);
    CHECK(client != NULL);
    if (!client) {
        fake_stop(f);
        return;
    }
    rc = resend_emails_cancel(client, id, &email);
    CHECK(rc == 0);
    CHECK(fake_method(f) != NULL && strcmp(fake_method(f), "POST") == 0);
    CHECK(fake_path(f) != NULL && strcmp(fake_path(f), "/emails/a%2Fb%20c%0A%3F/cancel") == 0);
    resend_email_free(&email);
    resend_free(client);
    fake_stop(f);

    f = fake_start(&exchange, 1);
    CHECK(f != NULL);
    if (!f)
        return;
    client = client_on(f);
    CHECK(client != NULL);
    if (!client) {
        fake_stop(f);
        return;
    }
    static const char *const dots[] = {".", "..", "%2e", "%2E%2e"};
    for (size_t i = 0; i < sizeof dots / sizeof dots[0]; i++) {
        memset(&email, 0, sizeof email);
        rc = resend_emails_get(client, dots[i], &email);
        CHECK(rc == -1);
        CHECK(email.id == NULL);
        const resend_error *err = resend_last_error(client);
        CHECK(err != NULL && err->status == 0);
        CHECK(err->message != NULL && strcmp(err->message, "id cannot be . or ..") == 0);
    }
    memset(&email, 0, sizeof email);
    rc = resend_emails_cancel(client, "..", &email);
    CHECK(rc == -1);
    CHECK(fake_request_count(f) == 0);
    resend_email_free(&email);
    resend_free(client);
    fake_stop(f);
}

/* NULL page has no query. Cursor values are percent-encoded. */
static void test_emails_list_query(void) {
    static const char body[] = "{\"has_more\":false,\"data\":[]}";
    fake_exchange exchange = {.status = 200, .body = body};
    fake *f = fake_start(&exchange, 1);
    CHECK(f != NULL);
    if (!f)
        return;
    resend *client = client_on(f);
    CHECK(client != NULL);
    if (!client) {
        fake_stop(f);
        return;
    }

    resend_email_list list = {0};
    int rc = resend_emails_list(client, NULL, &list);
    CHECK(rc == 0);
    CHECK(fake_path(f) != NULL && strcmp(fake_path(f), "/emails") == 0);
    CHECK(list.count == 0);
    CHECK(list.has_more == 0);
    resend_email_list_free(&list);
    resend_free(client);
    fake_stop(f);

    f = fake_start(&exchange, 1);
    CHECK(f != NULL);
    if (!f)
        return;
    client = client_on(f);
    CHECK(client != NULL);
    if (!client) {
        fake_stop(f);
        return;
    }
    resend_page page = {.after = "a b&c=d"};
    rc = resend_emails_list(client, &page, &list);
    CHECK(rc == 0);
    CHECK(fake_path(f) != NULL && strcmp(fake_path(f), "/emails?after=a%20b%26c%3Dd") == 0);
    resend_email_list_free(&list);
    resend_free(client);
    fake_stop(f);
}

/* Malformed JSON is an error. A real null, or a missing data member, is an empty page. */
static void test_emails_list_bad(void) {
    static const fake_exchange script[] = {
        {.status = 200, .body = "not-json"},
        {.status = 200, .body = "{\"has_more\":truefoo,\"data\":[]}"},
        {.status = 200, .body = "{\"has_more\":false,\"data\":nullfoo}"},
        {.status = 200, .body = "{\"has_more\":false,\"data\":[{\"subject\":\"only\"}]}"},
        {.status = 200, .body = "{\"has_more\":false,\"data\":[{\"id\":\"x\"},]}"},
        {.status = 200, .body = "{\"has_more\":false,\"data\":[1]}"},
        {.status = 200, .body = "{\"has_more\":false,\"data\":null}"},
        {.status = 200, .body = "{\"has_more\":true}"},
    };
    fake *f = fake_start(script, sizeof script / sizeof script[0]);
    CHECK(f != NULL);
    if (!f)
        return;
    resend *client = client_on(f);
    CHECK(client != NULL);
    if (!client) {
        fake_stop(f);
        return;
    }

    for (size_t i = 0; i < 6; i++) {
        resend_email_list list = {0};
        int rc = resend_emails_list(client, NULL, &list);
        CHECK(rc == -1);
        CHECK(list.items == NULL);
        CHECK(list.count == 0);
        const resend_error *err = resend_last_error(client);
        CHECK(err != NULL && err->status == 200);
        CHECK(err->message != NULL && err->message[0] != '\0');
        resend_email_list_free(&list);
    }

    resend_email_list empty = {0};
    CHECK(resend_emails_list(client, NULL, &empty) == 0);
    CHECK(empty.count == 0);
    CHECK(empty.items == NULL);
    CHECK(empty.has_more == 0);
    resend_email_list_free(&empty);

    CHECK(resend_emails_list(client, NULL, &empty) == 0);
    CHECK(empty.count == 0);
    CHECK(empty.has_more == 1);
    resend_email_list_free(&empty);

    CHECK(fake_request_count(f) == 8);
    resend_free(client);
    fake_stop(f);
}

static void test_emails_get_bad(void) {
    static const fake_exchange script[] = {
        {.status = 200, .body = "not-json"},
        {.status = 200, .body = "{\"id\":\"x\",\"subject\":\"\\uD800\"}"},
        {.status = 200, .body = "{\"subject\":\"no id\"}"},
        {.status = 200, .body = "{\"id\":\"x\",\"subject\":\"a\nb\"}"},
        {.status = 200, .body = "{\"id\":\"x\",\"to\":[\"a\",]}"},
        {.status = 200, .body = "{\"id\":\"x\",\"html\":truefoo}"},
        /* Form feed is isspace in C, not JSON whitespace. */
        {.status = 200, .body = "{\f\"id\":\"x\"}"},
        {.status = 200, .body = "{\"id\":\"x\",\"scheduled_at\":false}"},
        {.status = 200, .body = "{\"id\":\"x\",\"tags\":true}"},
        {.status = 200, .body = "{\"id\":\"x\",\"tags\":[{\"name\":1}]}"},
        {.status = 200, .body = "{\"id\":\"x\",\"tags\":[1]}"},
        {.status = 200,
         .body = "{\"id\":\"x\",\"tags\":[{\"name\":\"ok\",\"value\":\"1\"},{\"name\":1}]}"},
    };
    fake *f = fake_start(script, sizeof script / sizeof script[0]);
    CHECK(f != NULL);
    if (!f)
        return;
    resend *client = client_on(f);
    CHECK(client != NULL);
    if (!client) {
        fake_stop(f);
        return;
    }

    for (size_t i = 0; i < sizeof script / sizeof script[0]; i++) {
        resend_email email = {0};
        int rc = resend_emails_get(client, "x", &email);
        CHECK(rc == -1);
        CHECK(email.id == NULL);
        CHECK(email.subject == NULL);
        CHECK(email.html == NULL);
        const resend_error *err = resend_last_error(client);
        CHECK(err != NULL && err->status == 200);
        CHECK(err->message != NULL && err->message[0] != '\0');
        resend_email_free(&email);
    }
    CHECK(fake_request_count(f) == (int)(sizeof script / sizeof script[0]));
    resend_free(client);
    fake_stop(f);
}

/*
 * Suite signer: OpenSSL HMAC-SHA256 over id.timestamp.body, secret after whsec_.
 * Uses the current time (or an explicit unix timestamp).
 */
static char *sign_v1(const char *secret, const char *svix_id, long ts, const void *body,
                     size_t body_len) {
    if (!secret || strncmp(secret, "whsec_", 6) != 0)
        return NULL;
    const char *b64 = secret + 6;
    size_t b64_len = strlen(b64);
    size_t pad = (4 - (b64_len % 4)) % 4;
    char *padded = malloc(b64_len + pad + 1);
    if (!padded)
        return NULL;
    memcpy(padded, b64, b64_len);
    memset(padded + b64_len, '=', pad);
    padded[b64_len + pad] = '\0';

    size_t key_cap = ((b64_len + pad) / 4) * 3;
    unsigned char *key = malloc(key_cap);
    if (!key) {
        free(padded);
        return NULL;
    }
    int key_len = EVP_DecodeBlock(key, (unsigned char *)padded, (int)(b64_len + pad));
    free(padded);
    if (key_len < 0) {
        free(key);
        return NULL;
    }
    /* EVP_DecodeBlock counts a byte for every '=', including ones already in the secret. */
    size_t eq = 0;
    for (size_t i = 0; i < b64_len; i++) {
        if (b64[i] == '=')
            eq++;
    }
    if ((size_t)key_len < pad + eq) {
        free(key);
        return NULL;
    }
    key_len -= (int)(pad + eq);
    if (key_len == 0) {
        free(key);
        return NULL;
    }

    char ts_buf[32];
    snprintf(ts_buf, sizeof ts_buf, "%ld", ts);
    size_t id_len = strlen(svix_id);
    size_t ts_len = strlen(ts_buf);
    size_t signed_len = id_len + 1 + ts_len + 1 + body_len;
    unsigned char *signed_content = malloc(signed_len);
    if (!signed_content) {
        free(key);
        return NULL;
    }
    memcpy(signed_content, svix_id, id_len);
    signed_content[id_len] = '.';
    memcpy(signed_content + id_len + 1, ts_buf, ts_len);
    signed_content[id_len + 1 + ts_len] = '.';
    if (body_len)
        memcpy(signed_content + id_len + 1 + ts_len + 1, body, body_len);

    unsigned char md[EVP_MAX_MD_SIZE];
    unsigned int md_len = 0;
    if (!HMAC(EVP_sha256(), key, key_len, signed_content, signed_len, md, &md_len)) {
        free(key);
        free(signed_content);
        return NULL;
    }
    free(key);
    free(signed_content);

    char *b64_sig = malloc(((md_len + 2) / 3) * 4 + 1);
    if (!b64_sig)
        return NULL;
    EVP_EncodeBlock((unsigned char *)b64_sig, md, (int)md_len);

    size_t out_len = 3 + strlen(b64_sig);
    char *out = malloc(out_len + 1);
    if (!out) {
        free(b64_sig);
        return NULL;
    }
    snprintf(out, out_len + 1, "v1,%s", b64_sig);
    free(b64_sig);
    return out;
}

/* C strings cannot hold U+0000. The reader stores U+FFFD so a skipped field,
 * a stored field, or one email in a list does not fail the call.
 * A repeated key keeps the last value. */
static void test_json_nul(void) {
    static const char skipped[] = "{\"id\":\"x\",\"other\":\"\\u0000\"}";
    static const char stored[] = "{\"id\":\"x\",\"subject\":\"hi\\u0000there\"}";
    static const char page[] =
        "{\"has_more\":false,\"data\":["
        "{\"id\":\"a\"},"
        "{\"id\":\"b\",\"html\":\"x\\u0000y\"}"
        "]}";
    static const char dup[] = "{\"id\":\"first\",\"id\":\"last\"}";
    fake_exchange script[] = {
        {.status = 200, .body = skipped},
        {.status = 200, .body = stored},
        {.status = 200, .body = page},
        {.status = 200, .body = dup},
    };
    fake *f = fake_start(script, sizeof script / sizeof script[0]);
    CHECK(f != NULL);
    if (!f)
        return;
    resend *client = client_on(f);
    CHECK(client != NULL);
    if (!client) {
        fake_stop(f);
        return;
    }

    resend_email email = {0};
    int rc = resend_emails_get(client, "x", &email);
    CHECK(rc == 0);
    CHECK(email.id != NULL && strcmp(email.id, "x") == 0);
    resend_email_free(&email);

    rc = resend_emails_get(client, "x", &email);
    CHECK(rc == 0);
    CHECK(email.subject != NULL && strcmp(email.subject, "hi\xEF\xBF\xBDthere") == 0);
    resend_email_free(&email);

    resend_email_list list = {0};
    rc = resend_emails_list(client, NULL, &list);
    CHECK(rc == 0);
    CHECK(list.count == 2);
    CHECK(list.items != NULL);
    CHECK(list.items[0].id != NULL && strcmp(list.items[0].id, "a") == 0);
    CHECK(list.items[1].id != NULL && strcmp(list.items[1].id, "b") == 0);
    CHECK(list.items[1].html != NULL && strcmp(list.items[1].html, "x\xEF\xBF\xBDy") == 0);
    resend_email_list_free(&list);

    rc = resend_emails_get(client, "x", &email);
    CHECK(rc == 0);
    CHECK(email.id != NULL && strcmp(email.id, "last") == 0);
    resend_email_free(&email);

    CHECK(fake_request_count(f) == 4);
    resend_free(client);
    fake_stop(f);
}

static void test_webhook_verify(void) {
    /* Svix's published vector. The timestamp is outside the window, so this locks the signer. */
    static const char svix_body[] = "{\"test\": 2432232314}";
    char *sig = sign_v1("whsec_MfKQ9r8GKYqrTwjUPD8ILPZIo2LaLaSw", "msg_p5jXN8AQM9LWM0D4loKWxJek",
                        1614265330L, svix_body, strlen(svix_body));
    CHECK(sig != NULL && strcmp(sig, "v1,g0hM9SsE+OTPJTGt/tmIKtSyZlE3uFJELVlNIOLJ1OE=") == 0);
    free(sig);

    /* Two padding characters. EVP_DecodeBlock would otherwise keep two trailing NULs. */
    static const char yq_body[] =
        "{\"type\":\"email.sent\",\"created_at\":\"t\",\"data\":{\"email_id\":\"e\"}}";
    sig = sign_v1("whsec_YQ==", "msg_yq", 1700000000L, yq_body, strlen(yq_body));
    CHECK(sig != NULL && strcmp(sig, "v1,SfFUHX3Y4Bnn2LBRKkwM9crNPrjLoxPrNpll+CrGAJE=") == 0);
    free(sig);

    /* One padding character. */
    static const char pad_body[] =
        "{\"type\":\"email.sent\",\"created_at\":\"2024-02-15T12:00:00.000Z\","
        "\"data\":{\"email_id\":\"e_1\"}}";
    sig = sign_v1("whsec_cGFkLW1lISE=", "msg_pad", 1700000000L, pad_body, strlen(pad_body));
    CHECK(sig != NULL && strcmp(sig, "v1,OjqQX9hoA/0P9coI19pCCqq0s4Mk6TXkb0slY4HNzpA=") == 0);
    free(sig);

    static const char secret[] = "whsec_MfKQ9r8GKYqrTwjUPD8ILPZIo2LaLaSw";
    static const char body[] =
        "{\"type\":\"email.sent\",\"created_at\":\"2024-02-15T12:00:00.000Z\","
        "\"data\":{\"email_id\":\"571f1f42-1c2d-4b1f-8f8e-8b3b5b3b5b3b\","
        "\"from\":\"Acme <onboarding@resend.dev>\",\"to\":[\"you@example.com\"],"
        "\"subject\":\"Hello\",\"created_at\":\"2024-02-15T11:59:59.000Z\"}}";
    static const char *svix_id = "msg_2K6pQ8example";
    long now = (long)time(NULL);
    sig = sign_v1(secret, svix_id, now, body, strlen(body));
    CHECK(sig != NULL);
    if (!sig)
        return;

    char ts[32];
    snprintf(ts, sizeof ts, "%ld", now);

    resend_event event = {0};
    resend_error err = {0};
    int rc = resend_webhook_verify(secret, svix_id, ts, sig, body, strlen(body), &event, &err);
    CHECK(rc == 0);
    CHECK(event.type != NULL && strcmp(event.type, "email.sent") == 0);
    CHECK(event.created_at != NULL && strcmp(event.created_at, "2024-02-15T12:00:00.000Z") == 0);
    CHECK(event.email_id != NULL &&
          strcmp(event.email_id, "571f1f42-1c2d-4b1f-8f8e-8b3b5b3b5b3b") == 0);
    CHECK(event.data_json != NULL &&
          strcmp(event.data_json,
                 "{\"email_id\":\"571f1f42-1c2d-4b1f-8f8e-8b3b5b3b5b3b\","
                 "\"from\":\"Acme <onboarding@resend.dev>\",\"to\":[\"you@example.com\"],"
                 "\"subject\":\"Hello\",\"created_at\":\"2024-02-15T11:59:59.000Z\"}") == 0);
    resend_event_free(&event);
    free(sig);

    /* Prefix is optional; Svix and the other SDKs strip it when present. */
    now = (long)time(NULL);
    sig = sign_v1(secret, svix_id, now, body, strlen(body));
    CHECK(sig != NULL);
    if (!sig)
        return;
    snprintf(ts, sizeof ts, "%ld", now);
    memset(&event, 0, sizeof event);
    memset(&err, 0, sizeof err);
    rc = resend_webhook_verify(secret + 6, svix_id, ts, sig, body, strlen(body), &event, &err);
    CHECK(rc == 0);
    CHECK(event.type != NULL && strcmp(event.type, "email.sent") == 0);
    resend_event_free(&event);
    free(sig);

    /* No email_id in data → email_id stays NULL; data_json still filled. */
    static const char domain_body[] =
        "{\"type\":\"domain.created\",\"created_at\":\"2024-03-01T00:00:00.000Z\","
        "\"data\":{\"id\":\"d_1\",\"name\":\"example.com\"}}";
    now = (long)time(NULL);
    sig = sign_v1(secret, svix_id, now, domain_body, strlen(domain_body));
    CHECK(sig != NULL);
    if (!sig)
        return;
    snprintf(ts, sizeof ts, "%ld", now);
    memset(&event, 0, sizeof event);
    memset(&err, 0, sizeof err);
    rc = resend_webhook_verify(secret, svix_id, ts, sig, domain_body, strlen(domain_body), &event,
                               &err);
    CHECK(rc == 0);
    CHECK(event.type != NULL && strcmp(event.type, "domain.created") == 0);
    CHECK(event.created_at != NULL && strcmp(event.created_at, "2024-03-01T00:00:00.000Z") == 0);
    CHECK(event.email_id == NULL);
    CHECK(event.data_json != NULL &&
          strcmp(event.data_json, "{\"id\":\"d_1\",\"name\":\"example.com\"}") == 0);
    resend_event_free(&event);
    free(sig);

    /* created_at, email_id, and data_json are optional. */
    static const char type_only[] = "{\"type\":\"email.sent\"}";
    now = (long)time(NULL);
    sig = sign_v1(secret, svix_id, now, type_only, strlen(type_only));
    CHECK(sig != NULL);
    if (!sig)
        return;
    snprintf(ts, sizeof ts, "%ld", now);
    memset(&event, 0, sizeof event);
    memset(&err, 0, sizeof err);
    rc = resend_webhook_verify(secret, svix_id, ts, sig, type_only, strlen(type_only), &event, &err);
    CHECK(rc == 0);
    CHECK(event.type != NULL && strcmp(event.type, "email.sent") == 0);
    CHECK(event.created_at == NULL);
    CHECK(event.email_id == NULL);
    CHECK(event.data_json == NULL);
    resend_event_free(&event);
    free(sig);

    /* Second v1, value matches; first is decoy. */
    now = (long)time(NULL);
    sig = sign_v1(secret, svix_id, now, body, strlen(body));
    CHECK(sig != NULL);
    if (!sig)
        return;
    snprintf(ts, sizeof ts, "%ld", now);
    char multi[256];
    snprintf(multi, sizeof multi, "v1,AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA= %s", sig);
    memset(&event, 0, sizeof event);
    memset(&err, 0, sizeof err);
    rc = resend_webhook_verify(secret, svix_id, ts, multi, body, strlen(body), &event, &err);
    CHECK(rc == 0);
    CHECK(event.type != NULL && strcmp(event.type, "email.sent") == 0);
    resend_event_free(&event);
    free(sig);

    /* Bad signature. */
    now = (long)time(NULL);
    snprintf(ts, sizeof ts, "%ld", now);
    memset(&event, 0, sizeof event);
    memset(&err, 0, sizeof err);
    rc = resend_webhook_verify(secret, svix_id, ts, "v1,AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA=",
                               body, strlen(body), &event, &err);
    CHECK(rc == -1);
    CHECK(event.type == NULL);
    CHECK(err.message != NULL && err.message[0] != '\0');

    /* v2-only header is ignored → no matching v1. */
    memset(&event, 0, sizeof event);
    memset(&err, 0, sizeof err);
    rc = resend_webhook_verify(secret, svix_id, ts,
                               "v2,MzJsNDk4MzI0K2VvdSMjMTEjQEBAQDEyMzMzMzEyMwo=", body, strlen(body),
                               &event, &err);
    CHECK(rc == -1);

    /* Timestamp outside five minutes. */
    long stale = (long)time(NULL) - 301;
    sig = sign_v1(secret, svix_id, stale, body, strlen(body));
    CHECK(sig != NULL);
    if (!sig)
        return;
    snprintf(ts, sizeof ts, "%ld", stale);
    memset(&event, 0, sizeof event);
    memset(&err, 0, sizeof err);
    rc = resend_webhook_verify(secret, svix_id, ts, sig, body, strlen(body), &event, &err);
    CHECK(rc == -1);
    CHECK(event.type == NULL);
    free(sig);

    long future = (long)time(NULL) + 301;
    sig = sign_v1(secret, svix_id, future, body, strlen(body));
    CHECK(sig != NULL);
    if (!sig)
        return;
    snprintf(ts, sizeof ts, "%ld", future);
    memset(&event, 0, sizeof event);
    memset(&err, 0, sizeof err);
    rc = resend_webhook_verify(secret, svix_id, ts, sig, body, strlen(body), &event, &err);
    CHECK(rc == -1);
    CHECK(event.type == NULL);
    CHECK(strcmp(err.message, "timestamp outside tolerance") == 0);
    free(sig);

    /* Padded secret round-trip. The signer vector above is not passed through verify. */
    now = (long)time(NULL);
    sig = sign_v1("whsec_YQ==", "msg_yq", now, yq_body, strlen(yq_body));
    CHECK(sig != NULL);
    if (!sig)
        return;
    snprintf(ts, sizeof ts, "%ld", now);
    memset(&event, 0, sizeof event);
    memset(&err, 0, sizeof err);
    rc = resend_webhook_verify("whsec_YQ==", "msg_yq", ts, sig, yq_body, strlen(yq_body), &event,
                               &err);
    CHECK(rc == 0);
    CHECK(event.type != NULL && strcmp(event.type, "email.sent") == 0);
    CHECK(event.created_at != NULL && strcmp(event.created_at, "t") == 0);
    CHECK(event.email_id != NULL && strcmp(event.email_id, "e") == 0);
    CHECK(event.data_json != NULL && strcmp(event.data_json, "{\"email_id\":\"e\"}") == 0);
    resend_event_free(&event);
    free(sig);

    /* data.created_at and a nested email_id are not the event fields. */
    static const char ordered[] =
        "{\"type\":\"email.delivered\",\"data\":{\"created_at\":\"inner\","
        "\"nested\":{\"email_id\":\"nested-id\"},\"email_id\":\"real-id\"},"
        "\"created_at\":\"outer-time\"}";
    now = (long)time(NULL);
    sig = sign_v1(secret, svix_id, now, ordered, strlen(ordered));
    CHECK(sig != NULL);
    if (!sig)
        return;
    snprintf(ts, sizeof ts, "%ld", now);
    memset(&event, 0, sizeof event);
    memset(&err, 0, sizeof err);
    rc = resend_webhook_verify(secret, svix_id, ts, sig, ordered, strlen(ordered), &event, &err);
    CHECK(rc == 0);
    CHECK(event.type != NULL && strcmp(event.type, "email.delivered") == 0);
    CHECK(event.created_at != NULL && strcmp(event.created_at, "outer-time") == 0);
    CHECK(event.email_id != NULL && strcmp(event.email_id, "real-id") == 0);
    CHECK(event.data_json != NULL &&
          strcmp(event.data_json,
                 "{\"created_at\":\"inner\",\"nested\":{\"email_id\":\"nested-id\"},"
                 "\"email_id\":\"real-id\"}") == 0);
    resend_event_free(&event);
    free(sig);

    /* Repeated email_id: last wins; data_json keeps both. */
    static const char dup_body[] =
        "{\"type\":\"email.sent\",\"created_at\":\"t\","
        "\"data\":{\"email_id\":\"first\",\"email_id\":\"last\"}}";
    now = (long)time(NULL);
    sig = sign_v1(secret, svix_id, now, dup_body, strlen(dup_body));
    CHECK(sig != NULL);
    if (!sig)
        return;
    snprintf(ts, sizeof ts, "%ld", now);
    memset(&event, 0, sizeof event);
    memset(&err, 0, sizeof err);
    rc = resend_webhook_verify(secret, svix_id, ts, sig, dup_body, strlen(dup_body), &event, &err);
    CHECK(rc == 0);
    CHECK(event.email_id != NULL && strcmp(event.email_id, "last") == 0);
    CHECK(event.data_json != NULL &&
          strcmp(event.data_json, "{\"email_id\":\"first\",\"email_id\":\"last\"}") == 0);
    resend_event_free(&event);
    free(sig);

    /* Signature covers body_len bytes, and a different id or a flipped byte misses. */
    now = (long)time(NULL);
    char raw[sizeof body + 4];
    size_t n = strlen(body);
    memcpy(raw, body, n);
    memcpy(raw + n, "XXXX", 4);
    sig = sign_v1(secret, svix_id, now, raw, n);
    CHECK(sig != NULL);
    if (!sig)
        return;
    snprintf(ts, sizeof ts, "%ld", now);
    memset(&event, 0, sizeof event);
    rc = resend_webhook_verify(secret, svix_id, ts, sig, raw, n, &event, &err);
    CHECK(rc == 0);
    resend_event_free(&event);
    memset(&event, 0, sizeof event);
    rc = resend_webhook_verify(secret, svix_id, ts, sig, raw, n + 4, &event, &err);
    CHECK(rc == -1);
    CHECK(event.type == NULL);
    CHECK(strcmp(err.message, "invalid signature") == 0);
    raw[2] ^= 1;
    memset(&event, 0, sizeof event);
    rc = resend_webhook_verify(secret, svix_id, ts, sig, raw, n, &event, &err);
    CHECK(rc == -1);
    CHECK(strcmp(err.message, "invalid signature") == 0);
    memset(&event, 0, sizeof event);
    rc = resend_webhook_verify(secret, "msg_other", ts, sig, body, n, &event, &err);
    CHECK(rc == -1);
    CHECK(strcmp(err.message, "invalid signature") == 0);
    free(sig);

    /* Signed bytes that are not an event. */
    now = (long)time(NULL);
    sig = sign_v1(secret, svix_id, now, "[]", 2);
    CHECK(sig != NULL);
    if (!sig)
        return;
    snprintf(ts, sizeof ts, "%ld", now);
    memset(&event, 0, sizeof event);
    rc = resend_webhook_verify(secret, svix_id, ts, sig, "[]", 2, &event, &err);
    CHECK(rc == -1);
    CHECK(event.type == NULL);
    CHECK(event.data_json == NULL);
    CHECK(strcmp(err.message, "invalid payload") == 0);
    resend_event_free(&event);
    free(sig);

    snprintf(ts, sizeof ts, "%ld", (long)time(NULL));
    memset(&event, 0, sizeof event);
    rc = resend_webhook_verify("whsec_****", svix_id, ts, "v1,aa", body, strlen(body), &event,
                               &err);
    CHECK(rc == -1);
    CHECK(strcmp(err.message, "invalid secret") == 0);
    memset(&event, 0, sizeof event);
    rc = resend_webhook_verify("whsec_YQ=", svix_id, ts, "v1,aa", body, strlen(body), &event, &err);
    CHECK(rc == -1);
    CHECK(strcmp(err.message, "invalid secret") == 0);

    memset(&event, 0, sizeof event);
    rc = resend_webhook_verify(secret, svix_id, "9223372036854775807", "v1,aa", body, strlen(body),
                               &event, &err);
    CHECK(rc == -1);
    CHECK(event.type == NULL);
    CHECK(strcmp(err.message, "timestamp outside tolerance") == 0);
    /* Past the 32-bit long limit (2038-01-19); must parse as int64_t then fail the window. */
    memset(&event, 0, sizeof event);
    rc = resend_webhook_verify(secret, svix_id, "2147483648", "v1,aa", body, strlen(body), &event,
                               &err);
    CHECK(rc == -1);
    CHECK(event.type == NULL);
    CHECK(strcmp(err.message, "timestamp outside tolerance") == 0);
    memset(&event, 0, sizeof event);
    rc = resend_webhook_verify(secret, svix_id, "111111111111111111111", "v1,aa", body, strlen(body),
                               &event, &err);
    CHECK(rc == -1);
    CHECK(strcmp(err.message, "invalid timestamp") == 0);
    memset(&event, 0, sizeof event);
    rc = resend_webhook_verify(secret, svix_id, "-1", "v1,aa", body, strlen(body), &event, &err);
    CHECK(rc == -1);
    CHECK(strcmp(err.message, "invalid timestamp") == 0);
    memset(&event, 0, sizeof event);
    rc = resend_webhook_verify(secret, svix_id, "12ab", "v1,aa", body, strlen(body), &event, &err);
    CHECK(rc == -1);
    CHECK(strcmp(err.message, "invalid timestamp") == 0);
}

/*
 * GET /emails/receiving/{id}. html_format is data_uri, cid, or omitted.
 * Fills html, text, html_format, headers, received_for, authentication, and raw.
 * Attachment metadata on the email leaves download_url and expires_at NULL.
 */
static void test_receiving_get(void) {
    static const char id[] = "4ef9a417-02e9-4d39-ad75-9611e0fcc33c";
    static const char full[] =
        "{\"object\":\"email\","
        "\"id\":\"4ef9a417-02e9-4d39-ad75-9611e0fcc33c\","
        "\"to\":[\"delivered@resend.dev\"],"
        "\"from\":\"onboarding@resend.dev\","
        "\"created_at\":\"2026-04-03T22:13:42.674Z\","
        "\"subject\":\"Hello World\","
        "\"html\":\"Congrats on sending your <strong>first email</strong>!\","
        "\"html_format\":\"cid\","
        "\"text\":\"Congrats on sending your first email!\","
        "\"headers\":{"
        "\"from\":\"Acme <onboarding@resend.dev>\","
        "\"return-path\":\"lucas.costa@resend.com\","
        "\"mime-version\":\"1.0\""
        "},"
        "\"bcc\":[],"
        "\"cc\":[\"cc@example.com\"],"
        "\"reply_to\":[\"reply@example.com\"],"
        "\"received_for\":[\"forwarded@example.com\",\"also@example.com\"],"
        "\"authentication\":{\"spf\":\"pass\",\"dkim\":\"gray\",\"dmarc\":\"fail\",\"arc\":\"ignored\"},"
        "\"message_id\":\"<111-222-333@email.example.com>\","
        "\"raw\":{"
        "\"download_url\":\"https://example.resend.com/receiving/raw/054da427\","
        "\"expires_at\":\"2026-04-03T23:13:42.674Z\""
        "},"
        "\"attachments\":["
        "{\"id\":\"2a0c9ce0-3112-4728-976e-47ddcd16a318\","
        "\"filename\":\"avatar.png\",\"content_type\":\"image/png\","
        "\"content_disposition\":\"inline\",\"content_id\":\"img001\",\"size\":4096,"
        "\"download_url\":\"https://inbound-cdn.resend.com/should-not-copy\","
        "\"expires_at\":\"2026-10-17T14:29:41.521Z\"},"
        "{\"id\":\"3b1d0df1-4223-5839-087f-54eedd27b419\","
        "\"filename\":null,\"content_type\":\"application/pdf\","
        "\"content_disposition\":null,\"content_id\":null,\"size\":13264}"
        "],"
        "\"preview\":\"ignored\"}";

    fake_exchange exchange = {.status = 200, .body = full};
    fake *f = fake_start(&exchange, 1);
    CHECK(f != NULL);
    if (!f)
        return;
    resend *client = client_on(f);
    CHECK(client != NULL);
    if (!client) {
        fake_stop(f);
        return;
    }

    resend_received email = {0};
    int rc = resend_receiving_get(client, id, "raw", &email);
    CHECK(rc == -1);
    CHECK(email.id == NULL);
    CHECK(fake_request_count(f) == 0);
    const resend_error *err = resend_last_error(client);
    CHECK(err != NULL && err->status == 0);
    CHECK(err->message != NULL && err->message[0] != '\0');

    memset(&email, 0, sizeof email);
    rc = resend_receiving_get(client, id, "", &email);
    CHECK(rc == -1);
    CHECK(fake_request_count(f) == 0);
    err = resend_last_error(client);
    CHECK(err != NULL && err->status == 0);

    rc = resend_receiving_get(client, id, "cid", &email);
    CHECK(rc == 0);
    CHECK(fake_request_count(f) == 1);
    CHECK(fake_method(f) != NULL && strcmp(fake_method(f), "GET") == 0);
    CHECK(fake_path(f) != NULL &&
          strcmp(fake_path(f),
                 "/emails/receiving/4ef9a417-02e9-4d39-ad75-9611e0fcc33c?html_format=cid") == 0);
    CHECK(header_eq(f, "Authorization", "Bearer re_test"));
    CHECK(email.id != NULL && strcmp(email.id, id) == 0);
    CHECK(email.from != NULL && strcmp(email.from, "onboarding@resend.dev") == 0);
    CHECK(email.to_count == 1);
    CHECK(email.to != NULL && strcmp(email.to[0], "delivered@resend.dev") == 0);
    CHECK(email.cc_count == 1);
    CHECK(email.cc != NULL && strcmp(email.cc[0], "cc@example.com") == 0);
    CHECK(email.bcc_count == 0);
    CHECK(email.reply_to_count == 1);
    CHECK(email.reply_to != NULL && strcmp(email.reply_to[0], "reply@example.com") == 0);
    CHECK(email.subject != NULL && strcmp(email.subject, "Hello World") == 0);
    CHECK(email.message_id != NULL &&
          strcmp(email.message_id, "<111-222-333@email.example.com>") == 0);
    CHECK(email.html != NULL &&
          strcmp(email.html, "Congrats on sending your <strong>first email</strong>!") == 0);
    CHECK(email.html_format != NULL && strcmp(email.html_format, "cid") == 0);
    CHECK(email.text != NULL && strcmp(email.text, "Congrats on sending your first email!") == 0);
    CHECK(email.header_count == 3);
    CHECK(email.headers != NULL);
    CHECK(email.headers[0].name != NULL && strcmp(email.headers[0].name, "from") == 0);
    CHECK(email.headers[0].value != NULL &&
          strcmp(email.headers[0].value, "Acme <onboarding@resend.dev>") == 0);
    CHECK(email.headers[1].name != NULL && strcmp(email.headers[1].name, "return-path") == 0);
    CHECK(email.headers[1].value != NULL &&
          strcmp(email.headers[1].value, "lucas.costa@resend.com") == 0);
    CHECK(email.headers[2].name != NULL && strcmp(email.headers[2].name, "mime-version") == 0);
    CHECK(email.headers[2].value != NULL && strcmp(email.headers[2].value, "1.0") == 0);
    CHECK(email.received_for_count == 2);
    CHECK(email.received_for != NULL &&
          strcmp(email.received_for[0], "forwarded@example.com") == 0);
    CHECK(strcmp(email.received_for[1], "also@example.com") == 0);
    CHECK(email.authentication.spf != NULL && strcmp(email.authentication.spf, "pass") == 0);
    CHECK(email.authentication.dkim != NULL && strcmp(email.authentication.dkim, "gray") == 0);
    CHECK(email.authentication.dmarc != NULL && strcmp(email.authentication.dmarc, "fail") == 0);
    CHECK(email.raw.download_url != NULL &&
          strcmp(email.raw.download_url, "https://example.resend.com/receiving/raw/054da427") == 0);
    CHECK(email.raw.expires_at != NULL &&
          strcmp(email.raw.expires_at, "2026-04-03T23:13:42.674Z") == 0);
    CHECK(email.created_at != NULL && strcmp(email.created_at, "2026-04-03T22:13:42.674Z") == 0);
    CHECK(email.attachment_count == 2);
    CHECK(email.attachments != NULL);
    CHECK(email.attachments[0].id != NULL &&
          strcmp(email.attachments[0].id, "2a0c9ce0-3112-4728-976e-47ddcd16a318") == 0);
    CHECK(email.attachments[0].filename != NULL &&
          strcmp(email.attachments[0].filename, "avatar.png") == 0);
    CHECK(email.attachments[0].content_type != NULL &&
          strcmp(email.attachments[0].content_type, "image/png") == 0);
    CHECK(email.attachments[0].content_disposition != NULL &&
          strcmp(email.attachments[0].content_disposition, "inline") == 0);
    CHECK(email.attachments[0].content_id != NULL &&
          strcmp(email.attachments[0].content_id, "img001") == 0);
    CHECK(email.attachments[0].size == 4096);
    CHECK(email.attachments[0].download_url == NULL);
    CHECK(email.attachments[0].expires_at == NULL);
    CHECK(email.attachments[1].id != NULL &&
          strcmp(email.attachments[1].id, "3b1d0df1-4223-5839-087f-54eedd27b419") == 0);
    CHECK(email.attachments[1].filename == NULL);
    CHECK(email.attachments[1].content_disposition == NULL);
    CHECK(email.attachments[1].content_id == NULL);
    CHECK(email.attachments[1].size == 13264);
    CHECK(email.attachments[1].download_url == NULL);
    CHECK(email.attachments[1].expires_at == NULL);

    resend_received_free(&email);
    resend_free(client);
    fake_stop(f);

    static const char omitted_body[] =
        "{\"id\":\"4ef9a417-02e9-4d39-ad75-9611e0fcc33c\","
        "\"html\":\"<p>Hi</p>\",\"html_format\":\"data_uri\",\"text\":null,"
        "\"headers\":{\"subject\":\"Hello World\"},"
        "\"received_for\":[\"forwarded@example.com\"],"
        "\"authentication\":null,\"raw\":null,"
        "\"attachments\":[{\"id\":\"2a0c9ce0-3112-4728-976e-47ddcd16a318\","
        "\"filename\":\"avatar.png\",\"content_type\":\"image/png\","
        "\"content_disposition\":\"inline\",\"content_id\":\"img001\",\"size\":4096,"
        "\"download_url\":\"https://inbound-cdn.resend.com/should-not-copy\","
        "\"expires_at\":\"2026-10-17T14:29:41.521Z\"}]}";
    fake_exchange omitted = {.status = 200, .body = omitted_body};
    f = fake_start(&omitted, 1);
    CHECK(f != NULL);
    if (!f)
        return;
    client = client_on(f);
    CHECK(client != NULL);
    if (!client) {
        fake_stop(f);
        return;
    }
    memset(&email, 0, sizeof email);
    rc = resend_receiving_get(client, id, NULL, &email);
    CHECK(rc == 0);
    CHECK(fake_path(f) != NULL &&
          strcmp(fake_path(f), "/emails/receiving/4ef9a417-02e9-4d39-ad75-9611e0fcc33c") == 0);
    CHECK(email.html != NULL && strcmp(email.html, "<p>Hi</p>") == 0);
    CHECK(email.html_format != NULL && strcmp(email.html_format, "data_uri") == 0);
    CHECK(email.text == NULL);
    CHECK(email.header_count == 1);
    CHECK(email.headers != NULL && email.headers[0].name != NULL &&
          strcmp(email.headers[0].name, "subject") == 0);
    CHECK(email.headers[0].value != NULL && strcmp(email.headers[0].value, "Hello World") == 0);
    CHECK(email.received_for_count == 1);
    CHECK(email.received_for != NULL &&
          strcmp(email.received_for[0], "forwarded@example.com") == 0);
    CHECK(email.authentication.spf == NULL);
    CHECK(email.authentication.dkim == NULL);
    CHECK(email.authentication.dmarc == NULL);
    CHECK(email.raw.download_url == NULL);
    CHECK(email.raw.expires_at == NULL);
    CHECK(email.attachment_count == 1);
    CHECK(email.attachments != NULL);
    CHECK(email.attachments[0].download_url == NULL);
    CHECK(email.attachments[0].expires_at == NULL);
    CHECK(email.attachments[0].size == 4096);
    resend_received_free(&email);
    resend_free(client);
    fake_stop(f);

    /* Body html_format can be cid when data_uri was requested. */
    static const char cid_anyway[] =
        "{\"id\":\"4ef9a417-02e9-4d39-ad75-9611e0fcc33c\","
        "\"html\":\"cid:img001\",\"html_format\":\"cid\",\"text\":\"plain\"}";
    fake_exchange anyway = {.status = 200, .body = cid_anyway};
    f = fake_start(&anyway, 1);
    CHECK(f != NULL);
    if (!f)
        return;
    client = client_on(f);
    CHECK(client != NULL);
    if (!client) {
        fake_stop(f);
        return;
    }
    rc = resend_receiving_get(client, id, "data_uri", &email);
    CHECK(rc == 0);
    CHECK(fake_path(f) != NULL &&
          strcmp(fake_path(f),
                 "/emails/receiving/4ef9a417-02e9-4d39-ad75-9611e0fcc33c?html_format=data_uri") ==
              0);
    CHECK(email.html_format != NULL && strcmp(email.html_format, "cid") == 0);
    CHECK(email.html != NULL && strcmp(email.html, "cid:img001") == 0);
    CHECK(email.text != NULL && strcmp(email.text, "plain") == 0);
    CHECK(email.header_count == 0);
    CHECK(email.headers == NULL);
    CHECK(email.attachment_count == 0);
    CHECK(email.attachments == NULL);
    CHECK(email.raw.download_url == NULL);
    resend_received_free(&email);
    resend_free(client);
    fake_stop(f);

    static const fake_exchange bad[] = {
        {.status = 200, .body = "not-json"},
        {.status = 200, .body = "{\"html\":\"no id\"}"},
        {.status = 200, .body = "{\"id\":\"x\",\"headers\":{\"from\":1}}"},
        {.status = 200, .body = "{\"id\":\"x\",\"attachments\":[{\"id\":\"a\",\"size\":\"4\"}]}"},
        {.status = 200, .body = "{\"id\":\"x\",\"authentication\":[]}"},
        {.status = 200, .body = "{\"id\":\"x\",\"raw\":{\"download_url\":true}}"},
        {.status = 200, .body = "{\"id\":\"x\",\"received_for\":\"forwarded@example.com\"}"},
        {.status = 404, .body = "{\"name\":\"not_found\",\"message\":\"Email not found\"}"},
    };
    f = fake_start(bad, sizeof bad / sizeof bad[0]);
    CHECK(f != NULL);
    if (!f)
        return;
    client = client_on(f);
    CHECK(client != NULL);
    if (!client) {
        fake_stop(f);
        return;
    }
    for (size_t i = 0; i < 7; i++) {
        resend_received bad_email = {0};
        rc = resend_receiving_get(client, id, NULL, &bad_email);
        CHECK(rc == -1);
        CHECK(bad_email.id == NULL);
        CHECK(bad_email.html == NULL);
        CHECK(bad_email.headers == NULL);
        CHECK(bad_email.attachments == NULL);
        err = resend_last_error(client);
        CHECK(err != NULL && err->status == 200);
        CHECK(err->message != NULL && err->message[0] != '\0');
        resend_received_free(&bad_email);
    }
    memset(&email, 0, sizeof email);
    rc = resend_receiving_get(client, id, NULL, &email);
    CHECK(rc == -1);
    CHECK(email.id == NULL);
    err = resend_last_error(client);
    CHECK(err != NULL && err->status == 404);
    CHECK(err->name != NULL && strcmp(err->name, "not_found") == 0);
    CHECK(err->message != NULL && strcmp(err->message, "Email not found") == 0);
    resend_received_free(&email);
    CHECK(fake_request_count(f) == 8);
    resend_free(client);
    fake_stop(f);
}

static void test_receiving_body_cap(void) {
    enum { big = 1024 * 1024 + 1 };
    static const char prefix[] = "{\"id\":\"x\",\"html\":\"";
    char *raw = malloc((size_t)big + 1);
    CHECK(raw != NULL);
    if (!raw)
        return;
    memcpy(raw, prefix, sizeof prefix - 1);
    memset(raw + (sizeof prefix - 1), 'a', (size_t)big - (sizeof prefix - 1) - 2);
    raw[big - 2] = '"';
    raw[big - 1] = '}';
    raw[big] = '\0';

    fake_exchange exchange = {.status = 200, .body = raw};
    fake *f = fake_start(&exchange, 1);
    CHECK(f != NULL);
    if (!f) {
        free(raw);
        return;
    }
    resend *client = client_on(f);
    CHECK(client != NULL);
    if (!client) {
        fake_stop(f);
        free(raw);
        return;
    }

    resend_received email = {0};
    int rc = resend_receiving_get(client, "x", "data_uri", &email);
    CHECK(rc == 0);
    CHECK(email.id != NULL && strcmp(email.id, "x") == 0);
    CHECK(email.html != NULL && strlen(email.html) == (size_t)big - (sizeof prefix - 1) - 2);

    resend_received_free(&email);
    resend_free(client);
    fake_stop(f);
    free(raw);
}

/*
 * GET /emails/receiving. Envelope plus attachment metadata. has_more.
 * No html, text, headers, or raw (even when those keys appear in the body).
 */
static void test_receiving_list(void) {
    static const char body[] =
        "{\"object\":\"list\",\"has_more\":true,\"data\":["
        "{\"id\":\"4ef9a417-02e9-4d39-ad75-9611e0fcc33c\","
        "\"to\":[\"delivered@resend.dev\"],"
        "\"from\":\"onboarding@resend.dev\","
        "\"created_at\":\"2026-04-03T22:13:42.674Z\","
        "\"subject\":\"Hello World\","
        "\"bcc\":[],\"cc\":[\"cc@example.com\"],\"reply_to\":[\"reply@example.com\"],"
        "\"message_id\":\"<111-222-333@email.example.com>\","
        "\"html\":\"should-not-copy\","
        "\"html_format\":\"cid\","
        "\"text\":\"should-not-copy\","
        "\"headers\":{\"from\":\"Acme <onboarding@resend.dev>\"},"
        "\"received_for\":[\"forwarded@example.com\"],"
        "\"authentication\":{\"spf\":\"pass\",\"dkim\":\"pass\",\"dmarc\":\"pass\"},"
        "\"raw\":{\"download_url\":\"https://example.resend.com/raw\",\"expires_at\":\"2026-04-03T23:13:42.674Z\"},"
        "\"attachments\":["
        "{\"id\":\"2a0c9ce0-3112-4728-976e-47ddcd16a318\","
        "\"filename\":\"avatar.png\",\"content_type\":\"image/png\","
        "\"content_disposition\":\"inline\",\"content_id\":\"img001\",\"size\":4096,"
        "\"download_url\":\"https://inbound-cdn.resend.com/should-not-copy\","
        "\"expires_at\":\"2026-10-17T14:29:41.521Z\"}"
        "]},"
        "{\"id\":\"3a9f8c2b-1e5d-4f8a-9c7b-2d6e5f8a9c7b\","
        "\"to\":[\"user@example.com\"],"
        "\"from\":\"hello@example.com\","
        "\"created_at\":\"2026-04-03T21:45:12.345Z\","
        "\"subject\":\"Welcome\","
        "\"bcc\":null,\"cc\":null,\"reply_to\":null,"
        "\"message_id\":\"<444-555-666@email.example.com>\","
        "\"attachments\":[]}"
        "]}";

    fake_exchange exchange = {.status = 200, .body = body};
    fake *f = fake_start(&exchange, 1);
    CHECK(f != NULL);
    if (!f)
        return;
    resend *client = client_on(f);
    CHECK(client != NULL);
    if (!client) {
        fake_stop(f);
        return;
    }

    resend_page both = {.after = "a", .before = "b", .limit = 10};
    resend_received_list list = {0};
    int rc = resend_receiving_list(client, &both, &list);
    CHECK(rc == -1);
    CHECK(fake_request_count(f) == 0);
    const resend_error *err = resend_last_error(client);
    CHECK(err != NULL && err->status == 0);
    CHECK(err->message != NULL && err->message[0] != '\0');

    resend_page page = {.after = "cursor-1", .limit = 0};
    rc = resend_receiving_list(client, &page, &list);
    CHECK(rc == 0);
    CHECK(fake_request_count(f) == 1);
    CHECK(fake_method(f) != NULL && strcmp(fake_method(f), "GET") == 0);
    CHECK(fake_path(f) != NULL && strcmp(fake_path(f), "/emails/receiving?after=cursor-1") == 0);
    CHECK(header_eq(f, "Authorization", "Bearer re_test"));
    CHECK(list.has_more == 1);
    CHECK(list.count == 2);
    CHECK(list.items != NULL);

    CHECK(list.items[0].id != NULL &&
          strcmp(list.items[0].id, "4ef9a417-02e9-4d39-ad75-9611e0fcc33c") == 0);
    CHECK(list.items[0].from != NULL && strcmp(list.items[0].from, "onboarding@resend.dev") == 0);
    CHECK(list.items[0].to_count == 1);
    CHECK(list.items[0].to != NULL && strcmp(list.items[0].to[0], "delivered@resend.dev") == 0);
    CHECK(list.items[0].cc_count == 1);
    CHECK(list.items[0].cc != NULL && strcmp(list.items[0].cc[0], "cc@example.com") == 0);
    CHECK(list.items[0].bcc_count == 0);
    CHECK(list.items[0].reply_to_count == 1);
    CHECK(list.items[0].reply_to != NULL &&
          strcmp(list.items[0].reply_to[0], "reply@example.com") == 0);
    CHECK(list.items[0].subject != NULL && strcmp(list.items[0].subject, "Hello World") == 0);
    CHECK(list.items[0].message_id != NULL &&
          strcmp(list.items[0].message_id, "<111-222-333@email.example.com>") == 0);
    CHECK(list.items[0].created_at != NULL &&
          strcmp(list.items[0].created_at, "2026-04-03T22:13:42.674Z") == 0);
    CHECK(list.items[0].html == NULL);
    CHECK(list.items[0].html_format == NULL);
    CHECK(list.items[0].text == NULL);
    CHECK(list.items[0].headers == NULL);
    CHECK(list.items[0].header_count == 0);
    CHECK(list.items[0].received_for == NULL);
    CHECK(list.items[0].received_for_count == 0);
    CHECK(list.items[0].authentication.spf == NULL);
    CHECK(list.items[0].authentication.dkim == NULL);
    CHECK(list.items[0].authentication.dmarc == NULL);
    CHECK(list.items[0].raw.download_url == NULL);
    CHECK(list.items[0].raw.expires_at == NULL);
    CHECK(list.items[0].attachment_count == 1);
    CHECK(list.items[0].attachments != NULL);
    CHECK(list.items[0].attachments[0].id != NULL &&
          strcmp(list.items[0].attachments[0].id, "2a0c9ce0-3112-4728-976e-47ddcd16a318") == 0);
    CHECK(list.items[0].attachments[0].filename != NULL &&
          strcmp(list.items[0].attachments[0].filename, "avatar.png") == 0);
    CHECK(list.items[0].attachments[0].content_type != NULL &&
          strcmp(list.items[0].attachments[0].content_type, "image/png") == 0);
    CHECK(list.items[0].attachments[0].content_disposition != NULL &&
          strcmp(list.items[0].attachments[0].content_disposition, "inline") == 0);
    CHECK(list.items[0].attachments[0].content_id != NULL &&
          strcmp(list.items[0].attachments[0].content_id, "img001") == 0);
    CHECK(list.items[0].attachments[0].size == 4096);
    CHECK(list.items[0].attachments[0].download_url == NULL);
    CHECK(list.items[0].attachments[0].expires_at == NULL);

    CHECK(list.items[1].id != NULL &&
          strcmp(list.items[1].id, "3a9f8c2b-1e5d-4f8a-9c7b-2d6e5f8a9c7b") == 0);
    CHECK(list.items[1].from != NULL && strcmp(list.items[1].from, "hello@example.com") == 0);
    CHECK(list.items[1].to_count == 1);
    CHECK(list.items[1].subject != NULL && strcmp(list.items[1].subject, "Welcome") == 0);
    CHECK(list.items[1].message_id != NULL &&
          strcmp(list.items[1].message_id, "<444-555-666@email.example.com>") == 0);
    CHECK(list.items[1].bcc_count == 0);
    CHECK(list.items[1].cc_count == 0);
    CHECK(list.items[1].reply_to_count == 0);
    CHECK(list.items[1].html == NULL);
    CHECK(list.items[1].text == NULL);
    CHECK(list.items[1].headers == NULL);
    CHECK(list.items[1].raw.download_url == NULL);
    CHECK(list.items[1].attachment_count == 0);
    CHECK(list.items[1].attachments == NULL);

    resend_received_list_free(&list);
    resend_free(client);
    fake_stop(f);

    f = fake_start(&exchange, 1);
    CHECK(f != NULL);
    if (!f)
        return;
    client = client_on(f);
    CHECK(client != NULL);
    if (!client) {
        fake_stop(f);
        return;
    }
    resend_page limited = {.before = "cursor-2", .limit = 5};
    memset(&list, 0, sizeof list);
    rc = resend_receiving_list(client, &limited, &list);
    CHECK(rc == 0);
    CHECK(fake_path(f) != NULL &&
          strcmp(fake_path(f), "/emails/receiving?before=cursor-2&limit=5") == 0);
    resend_received_list_free(&list);
    resend_free(client);
    fake_stop(f);

    static const char empty_body[] = "{\"has_more\":false,\"data\":[]}";
    fake_exchange empty_ex = {.status = 200, .body = empty_body};
    f = fake_start(&empty_ex, 1);
    CHECK(f != NULL);
    if (!f)
        return;
    client = client_on(f);
    CHECK(client != NULL);
    if (!client) {
        fake_stop(f);
        return;
    }
    rc = resend_receiving_list(client, NULL, &list);
    CHECK(rc == 0);
    CHECK(fake_path(f) != NULL && strcmp(fake_path(f), "/emails/receiving") == 0);
    CHECK(list.count == 0);
    CHECK(list.items == NULL);
    CHECK(list.has_more == 0);
    resend_received_list_free(&list);
    resend_free(client);
    fake_stop(f);

    static const fake_exchange bad[] = {
        {.status = 200, .body = "not-json"},
        {.status = 200, .body = "{\"has_more\":truefoo,\"data\":[]}"},
        {.status = 200, .body = "{\"has_more\":false,\"data\":[{\"subject\":\"only\"}]}"},
        {.status = 200, .body = "{\"has_more\":false,\"data\":[{\"id\":\"x\"},]}"},
        {.status = 200, .body = "{\"has_more\":false,\"data\":[1]}"},
        {.status = 200, .body = "{\"has_more\":false,\"data\":[{\"id\":\"x\",\"attachments\":[{\"size\":1}]}]}"},
        {.status = 200, .body = "{\"has_more\":false,\"data\":null}"},
        {.status = 200, .body = "{\"has_more\":true}"},
        {.status = 401, .body = "{\"name\":\"restricted_api_key\",\"message\":\"This API key is restricted\"}"},
    };
    f = fake_start(bad, sizeof bad / sizeof bad[0]);
    CHECK(f != NULL);
    if (!f)
        return;
    client = client_on(f);
    CHECK(client != NULL);
    if (!client) {
        fake_stop(f);
        return;
    }
    for (size_t i = 0; i < 6; i++) {
        resend_received_list bad_list = {0};
        rc = resend_receiving_list(client, NULL, &bad_list);
        CHECK(rc == -1);
        CHECK(bad_list.items == NULL);
        CHECK(bad_list.count == 0);
        err = resend_last_error(client);
        CHECK(err != NULL && err->status == 200);
        CHECK(err->message != NULL && err->message[0] != '\0');
        resend_received_list_free(&bad_list);
    }

    resend_received_list empty = {0};
    CHECK(resend_receiving_list(client, NULL, &empty) == 0);
    CHECK(empty.count == 0);
    CHECK(empty.items == NULL);
    CHECK(empty.has_more == 0);
    resend_received_list_free(&empty);

    CHECK(resend_receiving_list(client, NULL, &empty) == 0);
    CHECK(empty.count == 0);
    CHECK(empty.has_more == 1);
    resend_received_list_free(&empty);

    memset(&list, 0, sizeof list);
    rc = resend_receiving_list(client, NULL, &list);
    CHECK(rc == -1);
    CHECK(list.items == NULL);
    err = resend_last_error(client);
    CHECK(err != NULL && err->status == 401);
    CHECK(err->name != NULL && strcmp(err->name, "restricted_api_key") == 0);
    resend_received_list_free(&list);
    CHECK(fake_request_count(f) == 9);
    resend_free(client);
    fake_stop(f);
}

/*
 * Attachment list/get set download_url and expires_at. List takes resend_page
 * and returns has_more. Path is under /emails/receiving/{id}/attachments.
 */
static void test_receiving_attachments(void) {
    static const char email_id[] = "4ef9a417-02e9-4d39-ad75-9611e0fcc33c";
    static const char att_id[] = "2a0c9ce0-3112-4728-976e-47ddcd16a318";
    static const char list_body[] =
        "{\"object\":\"list\",\"has_more\":true,\"data\":["
        "{\"id\":\"2a0c9ce0-3112-4728-976e-47ddcd16a318\","
        "\"filename\":\"avatar.png\",\"content_type\":\"image/png\","
        "\"content_disposition\":\"inline\",\"content_id\":\"img001\",\"size\":4096,"
        "\"download_url\":\"https://inbound-cdn.resend.com/avatar.png?sig=1\","
        "\"expires_at\":\"2026-10-17T14:29:41.521Z\"},"
        "{\"id\":\"3b1d0df1-4223-5839-087f-54eedd27b419\","
        "\"filename\":null,\"content_type\":\"application/pdf\","
        "\"content_disposition\":null,\"content_id\":null,\"size\":13264,"
        "\"download_url\":\"https://inbound-cdn.resend.com/doc.pdf?sig=2\","
        "\"expires_at\":\"2026-10-17T14:29:41.521Z\"}"
        "]}";
    static const char get_body[] =
        "{\"object\":\"attachment\","
        "\"id\":\"2a0c9ce0-3112-4728-976e-47ddcd16a318\","
        "\"filename\":\"avatar.png\",\"content_type\":\"image/png\","
        "\"content_disposition\":\"inline\",\"content_id\":\"img001\",\"size\":4096,"
        "\"download_url\":\"https://inbound-cdn.resend.com/avatar.png?sig=1\","
        "\"expires_at\":\"2026-10-17T14:29:41.521Z\"}";

    fake_exchange list_ex = {.status = 200, .body = list_body};
    fake *f = fake_start(&list_ex, 1);
    CHECK(f != NULL);
    if (!f)
        return;
    resend *client = client_on(f);
    CHECK(client != NULL);
    if (!client) {
        fake_stop(f);
        return;
    }

    resend_page both = {.after = "a", .before = "b"};
    resend_received_attachment_list list = {0};
    int rc = resend_receiving_attachments_list(client, email_id, &both, &list);
    CHECK(rc == -1);
    CHECK(fake_request_count(f) == 0);
    const resend_error *err = resend_last_error(client);
    CHECK(err != NULL && err->status == 0);

    rc = resend_receiving_attachments_list(client, "", NULL, &list);
    CHECK(rc == -1);
    CHECK(fake_request_count(f) == 0);

    resend_page page = {.after = "cursor-1", .limit = 0};
    rc = resend_receiving_attachments_list(client, email_id, &page, &list);
    CHECK(rc == 0);
    CHECK(fake_request_count(f) == 1);
    CHECK(fake_method(f) != NULL && strcmp(fake_method(f), "GET") == 0);
    CHECK(fake_path(f) != NULL &&
          strcmp(fake_path(f),
                 "/emails/receiving/4ef9a417-02e9-4d39-ad75-9611e0fcc33c/attachments"
                 "?after=cursor-1") == 0);
    CHECK(header_eq(f, "Authorization", "Bearer re_test"));
    CHECK(list.has_more == 1);
    CHECK(list.count == 2);
    CHECK(list.items != NULL);
    CHECK(list.items[0].id != NULL && strcmp(list.items[0].id, att_id) == 0);
    CHECK(list.items[0].filename != NULL && strcmp(list.items[0].filename, "avatar.png") == 0);
    CHECK(list.items[0].content_type != NULL &&
          strcmp(list.items[0].content_type, "image/png") == 0);
    CHECK(list.items[0].content_disposition != NULL &&
          strcmp(list.items[0].content_disposition, "inline") == 0);
    CHECK(list.items[0].content_id != NULL && strcmp(list.items[0].content_id, "img001") == 0);
    CHECK(list.items[0].size == 4096);
    CHECK(list.items[0].download_url != NULL &&
          strcmp(list.items[0].download_url, "https://inbound-cdn.resend.com/avatar.png?sig=1") ==
              0);
    CHECK(list.items[0].expires_at != NULL &&
          strcmp(list.items[0].expires_at, "2026-10-17T14:29:41.521Z") == 0);
    CHECK(list.items[1].id != NULL &&
          strcmp(list.items[1].id, "3b1d0df1-4223-5839-087f-54eedd27b419") == 0);
    CHECK(list.items[1].filename == NULL);
    CHECK(list.items[1].content_disposition == NULL);
    CHECK(list.items[1].content_id == NULL);
    CHECK(list.items[1].size == 13264);
    CHECK(list.items[1].download_url != NULL &&
          strcmp(list.items[1].download_url, "https://inbound-cdn.resend.com/doc.pdf?sig=2") == 0);
    CHECK(list.items[1].expires_at != NULL);
    resend_received_attachment_list_free(&list);
    resend_free(client);
    fake_stop(f);

    f = fake_start(&list_ex, 1);
    CHECK(f != NULL);
    if (!f)
        return;
    client = client_on(f);
    CHECK(client != NULL);
    if (!client) {
        fake_stop(f);
        return;
    }
    resend_page limited = {.before = "cursor-2", .limit = 5};
    memset(&list, 0, sizeof list);
    rc = resend_receiving_attachments_list(client, email_id, &limited, &list);
    CHECK(rc == 0);
    CHECK(fake_path(f) != NULL &&
          strcmp(fake_path(f),
                 "/emails/receiving/4ef9a417-02e9-4d39-ad75-9611e0fcc33c/attachments"
                 "?before=cursor-2&limit=5") == 0);
    resend_received_attachment_list_free(&list);
    resend_free(client);
    fake_stop(f);

    f = fake_start(&list_ex, 1);
    CHECK(f != NULL);
    if (!f)
        return;
    client = client_on(f);
    CHECK(client != NULL);
    if (!client) {
        fake_stop(f);
        return;
    }
    memset(&list, 0, sizeof list);
    rc = resend_receiving_attachments_list(client, email_id, NULL, &list);
    CHECK(rc == 0);
    CHECK(fake_path(f) != NULL &&
          strcmp(fake_path(f),
                 "/emails/receiving/4ef9a417-02e9-4d39-ad75-9611e0fcc33c/attachments") == 0);
    resend_received_attachment_list_free(&list);
    resend_free(client);
    fake_stop(f);

    fake_exchange get_ex = {.status = 200, .body = get_body};
    f = fake_start(&get_ex, 1);
    CHECK(f != NULL);
    if (!f)
        return;
    client = client_on(f);
    CHECK(client != NULL);
    if (!client) {
        fake_stop(f);
        return;
    }
    resend_received_attachment att = {0};
    rc = resend_receiving_attachment_get(client, email_id, "", &att);
    CHECK(rc == -1);
    CHECK(fake_request_count(f) == 0);
    rc = resend_receiving_attachment_get(client, "", att_id, &att);
    CHECK(rc == -1);
    CHECK(fake_request_count(f) == 0);

    rc = resend_receiving_attachment_get(client, email_id, att_id, &att);
    CHECK(rc == 0);
    CHECK(fake_path(f) != NULL &&
          strcmp(fake_path(f),
                 "/emails/receiving/4ef9a417-02e9-4d39-ad75-9611e0fcc33c/attachments/"
                 "2a0c9ce0-3112-4728-976e-47ddcd16a318") == 0);
    CHECK(att.id != NULL && strcmp(att.id, att_id) == 0);
    CHECK(att.filename != NULL && strcmp(att.filename, "avatar.png") == 0);
    CHECK(att.content_type != NULL && strcmp(att.content_type, "image/png") == 0);
    CHECK(att.content_disposition != NULL && strcmp(att.content_disposition, "inline") == 0);
    CHECK(att.content_id != NULL && strcmp(att.content_id, "img001") == 0);
    CHECK(att.size == 4096);
    CHECK(att.download_url != NULL &&
          strcmp(att.download_url, "https://inbound-cdn.resend.com/avatar.png?sig=1") == 0);
    CHECK(att.expires_at != NULL && strcmp(att.expires_at, "2026-10-17T14:29:41.521Z") == 0);
    resend_received_attachment_free(&att);
    resend_free(client);
    fake_stop(f);

    static const fake_exchange bad[] = {
        {.status = 200, .body = "not-json"},
        {.status = 200, .body = "{\"has_more\":false,\"data\":[{\"filename\":\"x\"}]}"},
        {.status = 200, .body = "{\"has_more\":false,\"data\":[{\"id\":\"x\",\"size\":\"4\"}]}"},
        {.status = 200, .body = "{\"id\":\"x\",\"download_url\":true}"},
        {.status = 404, .body = "{\"name\":\"not_found\",\"message\":\"Attachment not found\"}"},
    };
    f = fake_start(bad, sizeof bad / sizeof bad[0]);
    CHECK(f != NULL);
    if (!f)
        return;
    client = client_on(f);
    CHECK(client != NULL);
    if (!client) {
        fake_stop(f);
        return;
    }
    for (size_t i = 0; i < 3; i++) {
        resend_received_attachment_list bad_list = {0};
        rc = resend_receiving_attachments_list(client, email_id, NULL, &bad_list);
        CHECK(rc == -1);
        CHECK(bad_list.items == NULL);
        err = resend_last_error(client);
        CHECK(err != NULL && err->status == 200);
        resend_received_attachment_list_free(&bad_list);
    }
    memset(&att, 0, sizeof att);
    rc = resend_receiving_attachment_get(client, email_id, att_id, &att);
    CHECK(rc == -1);
    CHECK(att.id == NULL);
    err = resend_last_error(client);
    CHECK(err != NULL && err->status == 200);
    resend_received_attachment_free(&att);

    memset(&att, 0, sizeof att);
    rc = resend_receiving_attachment_get(client, email_id, att_id, &att);
    CHECK(rc == -1);
    err = resend_last_error(client);
    CHECK(err != NULL && err->status == 404);
    CHECK(err->name != NULL && strcmp(err->name, "not_found") == 0);
    resend_received_attachment_free(&att);
    CHECK(fake_request_count(f) == 5);
    resend_free(client);
    fake_stop(f);
}

/*
 * HTTPS only, checked before any request. The fake is plain HTTP, so a
 * successful download is not exercised here.
 */
static void test_download(void) {
    fake_exchange exchange = {.status = 200, .body = "secret-bytes"};
    fake *f = fake_start(&exchange, 1);
    CHECK(f != NULL);
    if (!f)
        return;
    resend *client = client_on(f);
    CHECK(client != NULL);
    if (!client) {
        fake_stop(f);
        return;
    }

    void *bytes = (void *)0x1;
    size_t len = 1;
    int rc = resend_download(client, NULL, &bytes, &len);
    CHECK(rc == -1);
    CHECK(bytes == NULL);
    CHECK(len == 0);

    bytes = (void *)0x1;
    len = 1;
    rc = resend_download(client, "http://example.com/x", &bytes, &len);
    CHECK(rc == -1);
    CHECK(bytes == NULL);
    CHECK(len == 0);
    const resend_error *err = resend_last_error(client);
    CHECK(err != NULL && err->status == 0);
    CHECK(err->message != NULL && strcmp(err->message, "download url must be https") == 0);
    CHECK(fake_request_count(f) == 0);

    bytes = (void *)0x1;
    len = 1;
    rc = resend_download(client, "https://example.com/\n", &bytes, &len);
    CHECK(rc == -1);
    CHECK(bytes == NULL);
    CHECK(len == 0);
    err = resend_last_error(client);
    CHECK(err != NULL && err->status == 0);
    CHECK(err->message != NULL && strcmp(err->message, "invalid download url") == 0);
    CHECK(fake_request_count(f) == 0);

    resend_free(client);
    fake_stop(f);
}

/*
 * GET /domains/{id}. Records carry record, name, type, value, status, and ttl.
 * priority is -1 when the field is missing or null; 0 is a real MX priority.
 * sending and receiving are the capabilities strings ("enabled" or "disabled").
 * tracking_subdomain is set on get. Unknown fields are ignored.
 */
static void test_domains_get(void) {
    static const char id[] = "d91cd9bd-1176-453e-8fc1-35364d380206";
    static const char body[] =
        "{\"object\":\"domain\","
        "\"id\":\"d91cd9bd-1176-453e-8fc1-35364d380206\","
        "\"name\":\"example.com\","
        "\"status\":\"not_started\","
        "\"created_at\":\"2023-04-26 20:21:26.347412+00\","
        "\"region\":\"us-east-1\","
        "\"open_tracking\":true,"
        "\"click_tracking\":false,"
        "\"tracking_subdomain\":\"links\","
        "\"custom_return_path\":\"send\","
        "\"capabilities\":{\"sending\":\"enabled\",\"receiving\":\"disabled\",\"extra\":1},"
        "\"records\":["
        "{\"record\":\"SPF\",\"name\":\"send\",\"type\":\"MX\",\"ttl\":\"Auto\","
        "\"status\":\"not_started\",\"value\":\"feedback-smtp.us-east-1.amazonses.com\","
        "\"priority\":10,\"ignored\":true},"
        "{\"record\":\"SPF\",\"name\":\"send\","
        "\"value\":\"\\\"v=spf1 include:amazonses.com ~all\\\"\","
        "\"type\":\"TXT\",\"ttl\":\"Auto\",\"status\":\"not_started\"},"
        "{\"record\":\"DKIM\",\"name\":\"resend._domainkey\",\"value\":\"p=MIGfMA0\","
        "\"type\":\"TXT\",\"status\":\"not_started\",\"ttl\":\"Auto\",\"priority\":null},"
        "{\"record\":\"Receiving\",\"name\":\"example.com\","
        "\"value\":\"inbound-smtp.us-east-1.amazonaws.com\",\"type\":\"MX\","
        "\"ttl\":\"Auto\",\"status\":\"verified\",\"priority\":0},"
        "{\"record\":\"Tracking\",\"name\":\"links.example.com\",\"type\":\"CNAME\","
        "\"value\":\"links1.resend-dns.com\",\"ttl\":\"Auto\",\"status\":\"temporary_failure\"},"
        "{\"record\":\"TrackingCAA\",\"name\":\"links.example.com\","
        "\"value\":\"0 issue \\\"pki.resend.com\\\"\",\"type\":\"CAA\",\"ttl\":\"Auto\","
        "\"status\":\"failed\"}"
        "]}";

    resend_domain domain = {0};
    CHECK(resend_domains_get(NULL, id, &domain) == -1);

    fake_exchange exchange = {.status = 200, .body = body};
    fake *f = fake_start(&exchange, 1);
    CHECK(f != NULL);
    if (!f)
        return;
    resend *client = client_on(f);
    CHECK(client != NULL);
    if (!client) {
        fake_stop(f);
        return;
    }

    int rc = resend_domains_get(client, "", &domain);
    CHECK(rc == -1);
    CHECK(fake_request_count(f) == 0);
    rc = resend_domains_get(client, id, NULL);
    CHECK(rc == -1);
    CHECK(fake_request_count(f) == 0);

    rc = resend_domains_get(client, id, &domain);
    CHECK(rc == 0);
    CHECK(fake_request_count(f) == 1);
    CHECK(fake_method(f) != NULL && strcmp(fake_method(f), "GET") == 0);
    CHECK(fake_path(f) != NULL &&
          strcmp(fake_path(f), "/domains/d91cd9bd-1176-453e-8fc1-35364d380206") == 0);
    CHECK(header_eq(f, "Authorization", "Bearer re_test"));
    CHECK(fake_body(f) == NULL || fake_body(f)[0] == '\0');
    CHECK(domain.id != NULL && strcmp(domain.id, id) == 0);
    CHECK(domain.name != NULL && strcmp(domain.name, "example.com") == 0);
    CHECK(domain.status != NULL && strcmp(domain.status, "not_started") == 0);
    CHECK(domain.region != NULL && strcmp(domain.region, "us-east-1") == 0);
    CHECK(domain.created_at != NULL &&
          strcmp(domain.created_at, "2023-04-26 20:21:26.347412+00") == 0);
    CHECK(domain.open_tracking == 1);
    CHECK(domain.click_tracking == 0);
    CHECK(domain.tracking_subdomain != NULL && strcmp(domain.tracking_subdomain, "links") == 0);
    CHECK(domain.sending != NULL && strcmp(domain.sending, "enabled") == 0);
    CHECK(domain.receiving != NULL && strcmp(domain.receiving, "disabled") == 0);
    CHECK(domain.records != NULL && domain.record_count == 6);
    CHECK(domain.records != NULL && domain.record_count > 0 && domain.records[0].record != NULL &&
          strcmp(domain.records[0].record, "SPF") == 0);
    CHECK(domain.records != NULL && domain.record_count > 0 && domain.records[0].name != NULL &&
          strcmp(domain.records[0].name, "send") == 0);
    CHECK(domain.records != NULL && domain.record_count > 0 && domain.records[0].type != NULL &&
          strcmp(domain.records[0].type, "MX") == 0);
    CHECK(domain.records != NULL && domain.record_count > 0 && domain.records[0].value != NULL &&
          strcmp(domain.records[0].value, "feedback-smtp.us-east-1.amazonses.com") == 0);
    CHECK(domain.records != NULL && domain.record_count > 0 && domain.records[0].status != NULL &&
          strcmp(domain.records[0].status, "not_started") == 0);
    CHECK(domain.records != NULL && domain.record_count > 0 && domain.records[0].ttl != NULL &&
          strcmp(domain.records[0].ttl, "Auto") == 0);
    CHECK(domain.records != NULL && domain.record_count > 0 && domain.records[0].priority == 10);
    CHECK(domain.records != NULL && domain.record_count > 1 && domain.records[1].value != NULL &&
          strcmp(domain.records[1].value, "\"v=spf1 include:amazonses.com ~all\"") == 0);
    CHECK(domain.records != NULL && domain.record_count > 1 && domain.records[1].type != NULL &&
          strcmp(domain.records[1].type, "TXT") == 0);
    CHECK(domain.records != NULL && domain.record_count > 1 && domain.records[1].priority == -1);
    CHECK(domain.records != NULL && domain.record_count > 2 && domain.records[2].record != NULL &&
          strcmp(domain.records[2].record, "DKIM") == 0);
    CHECK(domain.records != NULL && domain.record_count > 2 && domain.records[2].name != NULL &&
          strcmp(domain.records[2].name, "resend._domainkey") == 0);
    CHECK(domain.records != NULL && domain.record_count > 2 && domain.records[2].priority == -1);
    CHECK(domain.records != NULL && domain.record_count > 3 && domain.records[3].record != NULL &&
          strcmp(domain.records[3].record, "Receiving") == 0);
    CHECK(domain.records != NULL && domain.record_count > 3 && domain.records[3].type != NULL &&
          strcmp(domain.records[3].type, "MX") == 0);
    CHECK(domain.records != NULL && domain.record_count > 3 && domain.records[3].status != NULL &&
          strcmp(domain.records[3].status, "verified") == 0);
    CHECK(domain.records != NULL && domain.record_count > 3 && domain.records[3].priority == 0);
    CHECK(domain.records != NULL && domain.record_count > 4 && domain.records[4].record != NULL &&
          strcmp(domain.records[4].record, "Tracking") == 0);
    CHECK(domain.records != NULL && domain.record_count > 4 && domain.records[4].type != NULL &&
          strcmp(domain.records[4].type, "CNAME") == 0);
    CHECK(domain.records != NULL && domain.record_count > 4 && domain.records[4].status != NULL &&
          strcmp(domain.records[4].status, "temporary_failure") == 0);
    CHECK(domain.records != NULL && domain.record_count > 4 && domain.records[4].priority == -1);
    CHECK(domain.records != NULL && domain.record_count > 5 && domain.records[5].record != NULL &&
          strcmp(domain.records[5].record, "TrackingCAA") == 0);
    CHECK(domain.records != NULL && domain.record_count > 5 && domain.records[5].type != NULL &&
          strcmp(domain.records[5].type, "CAA") == 0);
    CHECK(domain.records != NULL && domain.record_count > 5 && domain.records[5].value != NULL &&
          strcmp(domain.records[5].value, "0 issue \"pki.resend.com\"") == 0);
    CHECK(domain.records != NULL && domain.record_count > 5 && domain.records[5].status != NULL &&
          strcmp(domain.records[5].status, "failed") == 0);
    CHECK(domain.records != NULL && domain.record_count > 5 && domain.records[5].ttl != NULL &&
          strcmp(domain.records[5].ttl, "Auto") == 0);
    resend_domain_free(&domain);
    CHECK(domain.id == NULL);
    CHECK(domain.records == NULL);
    CHECK(domain.record_count == 0);
    CHECK(domain.sending == NULL);
    CHECK(domain.tracking_subdomain == NULL);
    resend_domain_free(&domain);
    resend_domain_free(NULL);
    resend_free(client);
    fake_stop(f);

    static const char absent_body[] =
        "{\"id\":\"d91cd9bd-1176-453e-8fc1-35364d380206\",\"name\":\"example.com\"}";
    fake_exchange absent = {.status = 200, .body = absent_body};
    f = fake_start(&absent, 1);
    CHECK(f != NULL);
    if (!f)
        return;
    client = client_on(f);
    CHECK(client != NULL);
    if (!client) {
        fake_stop(f);
        return;
    }
    rc = resend_domains_get(client, id, &domain);
    CHECK(rc == 0);
    CHECK(fake_path(f) != NULL &&
          strcmp(fake_path(f), "/domains/d91cd9bd-1176-453e-8fc1-35364d380206") == 0);
    CHECK(domain.id != NULL && strcmp(domain.id, id) == 0);
    CHECK(domain.name != NULL && strcmp(domain.name, "example.com") == 0);
    CHECK(domain.status == NULL);
    CHECK(domain.region == NULL);
    CHECK(domain.created_at == NULL);
    CHECK(domain.open_tracking == 0);
    CHECK(domain.click_tracking == 0);
    CHECK(domain.tracking_subdomain == NULL);
    CHECK(domain.sending == NULL);
    CHECK(domain.receiving == NULL);
    CHECK(domain.records == NULL);
    CHECK(domain.record_count == 0);
    resend_domain_free(&domain);
    resend_free(client);
    fake_stop(f);

    static const char nulls_body[] =
        "{\"id\":\"d91cd9bd-1176-453e-8fc1-35364d380206\","
        "\"name\":null,\"status\":null,\"region\":null,\"created_at\":null,"
        "\"open_tracking\":false,\"click_tracking\":null,\"tracking_subdomain\":null,"
        "\"capabilities\":null,\"records\":[]}";
    fake_exchange nulls = {.status = 200, .body = nulls_body};
    f = fake_start(&nulls, 1);
    CHECK(f != NULL);
    if (!f)
        return;
    client = client_on(f);
    CHECK(client != NULL);
    if (!client) {
        fake_stop(f);
        return;
    }
    rc = resend_domains_get(client, id, &domain);
    CHECK(rc == 0);
    CHECK(domain.id != NULL && strcmp(domain.id, id) == 0);
    CHECK(domain.name == NULL);
    CHECK(domain.status == NULL);
    CHECK(domain.region == NULL);
    CHECK(domain.created_at == NULL);
    CHECK(domain.open_tracking == 0);
    CHECK(domain.click_tracking == 0);
    CHECK(domain.tracking_subdomain == NULL);
    CHECK(domain.sending == NULL);
    CHECK(domain.receiving == NULL);
    CHECK(domain.records == NULL);
    CHECK(domain.record_count == 0);
    resend_domain_free(&domain);
    resend_free(client);
    fake_stop(f);

    static const char swapped_body[] =
        "{\"id\":\"d1\",\"tracking_subdomain\":\"click\",\"open_tracking\":true,"
        "\"click_tracking\":true,"
        "\"capabilities\":{\"sending\":\"disabled\",\"receiving\":\"enabled\"},"
        "\"records\":[{},{\"record\":\"SPF\",\"priority\":null,\"ttl\":null}]}";
    fake_exchange swapped = {.status = 200, .body = swapped_body};
    f = fake_start(&swapped, 1);
    CHECK(f != NULL);
    if (!f)
        return;
    client = client_on(f);
    CHECK(client != NULL);
    if (!client) {
        fake_stop(f);
        return;
    }
    rc = resend_domains_get(client, "d1", &domain);
    CHECK(rc == 0);
    CHECK(fake_path(f) != NULL && strcmp(fake_path(f), "/domains/d1") == 0);
    CHECK(domain.id != NULL && strcmp(domain.id, "d1") == 0);
    CHECK(domain.tracking_subdomain != NULL && strcmp(domain.tracking_subdomain, "click") == 0);
    CHECK(domain.open_tracking == 1);
    CHECK(domain.click_tracking == 1);
    CHECK(domain.sending != NULL && strcmp(domain.sending, "disabled") == 0);
    CHECK(domain.receiving != NULL && strcmp(domain.receiving, "enabled") == 0);
    CHECK(domain.records != NULL && domain.record_count == 2);
    CHECK(domain.records != NULL && domain.record_count > 0 && domain.records[0].record == NULL);
    CHECK(domain.records != NULL && domain.record_count > 0 && domain.records[0].name == NULL);
    CHECK(domain.records != NULL && domain.record_count > 0 && domain.records[0].type == NULL);
    CHECK(domain.records != NULL && domain.record_count > 0 && domain.records[0].value == NULL);
    CHECK(domain.records != NULL && domain.record_count > 0 && domain.records[0].status == NULL);
    CHECK(domain.records != NULL && domain.record_count > 0 && domain.records[0].ttl == NULL);
    CHECK(domain.records != NULL && domain.record_count > 0 && domain.records[0].priority == -1);
    CHECK(domain.records != NULL && domain.record_count > 1 && domain.records[1].record != NULL &&
          strcmp(domain.records[1].record, "SPF") == 0);
    CHECK(domain.records != NULL && domain.record_count > 1 && domain.records[1].ttl == NULL);
    CHECK(domain.records != NULL && domain.record_count > 1 && domain.records[1].priority == -1);
    resend_domain_free(&domain);
    resend_free(client);
    fake_stop(f);

    static const char sending_only_body[] =
        "{\"id\":\"d1\",\"capabilities\":{\"sending\":\"enabled\"}}";
    fake_exchange sending_only = {.status = 200, .body = sending_only_body};
    f = fake_start(&sending_only, 1);
    CHECK(f != NULL);
    if (!f)
        return;
    client = client_on(f);
    CHECK(client != NULL);
    if (!client) {
        fake_stop(f);
        return;
    }
    rc = resend_domains_get(client, "d1", &domain);
    CHECK(rc == 0);
    CHECK(domain.sending != NULL && strcmp(domain.sending, "enabled") == 0);
    CHECK(domain.receiving == NULL);
    CHECK(domain.records == NULL);
    CHECK(domain.record_count == 0);
    resend_domain_free(&domain);
    resend_free(client);
    fake_stop(f);

    static const char bounds_body[] =
        "{\"id\":\"bounds\",\"records\":[{\"priority\":2147483647},{\"priority\":-2147483648}]}";
    fake_exchange bounds = {.status = 200, .body = bounds_body};
    f = fake_start(&bounds, 1);
    CHECK(f != NULL);
    if (!f)
        return;
    client = client_on(f);
    CHECK(client != NULL);
    if (!client) {
        fake_stop(f);
        return;
    }
    rc = resend_domains_get(client, "bounds", &domain);
    CHECK(rc == 0);
    CHECK(domain.records != NULL && domain.record_count == 2);
    CHECK(domain.records != NULL && domain.record_count > 0 &&
          domain.records[0].priority == 2147483647);
    CHECK(domain.records != NULL && domain.record_count > 1 &&
          domain.records[1].priority == -2147483647 - 1);
    resend_domain_free(&domain);
    resend_free(client);
    fake_stop(f);

    static const char encoded_id[] = "a/b c\n?";
    static const char encoded_body[] =
        "{\"id\":\"a/b c\\n?\",\"tracking_subdomain\":\"links\"}";
    fake_exchange encoded = {.status = 200, .body = encoded_body};
    f = fake_start(&encoded, 1);
    CHECK(f != NULL);
    if (!f)
        return;
    client = client_on(f);
    CHECK(client != NULL);
    if (!client) {
        fake_stop(f);
        return;
    }
    rc = resend_domains_get(client, encoded_id, &domain);
    CHECK(rc == 0);
    CHECK(fake_path(f) != NULL && strcmp(fake_path(f), "/domains/a%2Fb%20c%0A%3F") == 0);
    CHECK(domain.id != NULL && strcmp(domain.id, encoded_id) == 0);
    CHECK(domain.tracking_subdomain != NULL && strcmp(domain.tracking_subdomain, "links") == 0);
    CHECK(domain.records == NULL);
    CHECK(domain.sending == NULL);
    resend_domain_free(&domain);
    resend_free(client);
    fake_stop(f);

    static const fake_exchange bad[] = {
        {.status = 200, .body = "not-json"},
        {.status = 200, .body = "null"},
        {.status = 200, .body = "[]"},
        {.status = 200, .body = "{\"name\":\"example.com\"}"},
        {.status = 200, .body = "{\"id\":null}"},
        {.status = 200, .body = "{\"id\":\"x\",\"records\":{}}"},
        {.status = 200, .body = "{\"id\":\"x\",\"records\":[null]}"},
        {.status = 200, .body = "{\"id\":\"x\",\"records\":[{\"priority\":\"10\"}]}"},
        {.status = 200, .body = "{\"id\":\"x\",\"records\":[{\"priority\":10.5}]}"},
        {.status = 200, .body = "{\"id\":\"x\",\"records\":[{\"priority\":2147483648}]}"},
        {.status = 200, .body = "{\"id\":\"x\",\"records\":[{\"priority\":-2147483649}]}"},
        {.status = 200,
         .body = "{\"id\":\"x\",\"name\":\"example.com\",\"records\":["
                 "{\"record\":\"SPF\",\"name\":\"send\",\"priority\":10},{\"ttl\":1}]}"},
        {.status = 200, .body = "{\"id\":\"x\",\"capabilities\":[]}"},
        {.status = 200,
         .body = "{\"id\":\"x\",\"capabilities\":{\"sending\":true,\"receiving\":\"disabled\"}}"},
        {.status = 200, .body = "{\"id\":\"x\",\"tracking_subdomain\":1}"},
        {.status = 200, .body = "{\"id\":\"x\",\"open_tracking\":\"true\"}"},
        {.status = 200, .body = "{\"id\":\"x\",\"click_tracking\":1}"},
        {.status = 200, .body = "{\"id\":\"x\",\"records\":[{\"name\":\"a\",}]}"},
        {.status = 200, .body = "{\"id\":\"x\",\"region\":true}"},
        {.status = 404,
         .body = "{\"name\":\"not_found\",\"message\":\"Domain not found\","
                 "\"id\":\"should-not-parse\"}"},
    };
    f = fake_start(bad, sizeof bad / sizeof bad[0]);
    CHECK(f != NULL);
    if (!f)
        return;
    client = client_on(f);
    CHECK(client != NULL);
    if (!client) {
        fake_stop(f);
        return;
    }
    for (size_t i = 0; i + 1 < sizeof bad / sizeof bad[0]; i++) {
        memset(&domain, 0x5a, sizeof domain);
        rc = resend_domains_get(client, "x", &domain);
        CHECK(rc == -1);
        CHECK(domain.id == NULL);
        CHECK(domain.name == NULL);
        CHECK(domain.sending == NULL);
        CHECK(domain.receiving == NULL);
        CHECK(domain.tracking_subdomain == NULL);
        CHECK(domain.records == NULL);
        CHECK(domain.record_count == 0);
        const resend_error *err = resend_last_error(client);
        CHECK(err != NULL && err->status == 200);
        CHECK(err->message != NULL && err->message[0] != '\0');
        resend_domain_free(&domain);
    }
    memset(&domain, 0x5a, sizeof domain);
    rc = resend_domains_get(client, id, &domain);
    CHECK(rc == -1);
    CHECK(domain.id == NULL);
    CHECK(domain.records == NULL);
    const resend_error *err = resend_last_error(client);
    CHECK(err != NULL && err->status == 404);
    CHECK(err->name != NULL && strcmp(err->name, "not_found") == 0);
    CHECK(err->message != NULL && strcmp(err->message, "Domain not found") == 0);
    resend_domain_free(&domain);
    CHECK(fake_request_count(f) == (int)(sizeof bad / sizeof bad[0]));
    resend_free(client);
    fake_stop(f);
}

/*
 * GET /domains. Page helper and has_more. records and tracking_subdomain stay
 * NULL even when the payload carries them.
 */
static void test_domains_list(void) {
    static const char id[] = "d91cd9bd-1176-453e-8fc1-35364d380206";
    static const char body[] =
        "{\"object\":\"list\",\"has_more\":true,\"data\":["
        "{\"id\":\"d91cd9bd-1176-453e-8fc1-35364d380206\","
        "\"name\":\"example.com\",\"status\":\"verified\",\"region\":\"eu-west-1\","
        "\"created_at\":\"2023-04-26 20:21:26.347412+00\","
        "\"open_tracking\":false,\"click_tracking\":true,"
        "\"tracking_subdomain\":1,"
        "\"capabilities\":{\"sending\":\"enabled\",\"receiving\":\"disabled\",\"extra\":true},"
        "\"records\":[{\"record\":\"SPF\",\"name\":\"send\",\"type\":\"MX\","
        "\"value\":\"should-not-copy\",\"status\":\"not_started\",\"ttl\":\"Auto\",\"priority\":10}]},"
        "{\"id\":\"d2\",\"name\":\"other.example\",\"status\":\"pending\","
        "\"region\":\"us-east-1\",\"created_at\":\"2024-01-01 00:00:00.000+00\"}"
        "]}";

    resend_domain_list list = {0};
    CHECK(resend_domains_list(NULL, NULL, &list) == -1);

    fake_exchange exchange = {.status = 200, .body = body};
    fake *f = fake_start(&exchange, 1);
    CHECK(f != NULL);
    if (!f)
        return;
    resend *client = client_on(f);
    CHECK(client != NULL);
    if (!client) {
        fake_stop(f);
        return;
    }

    resend_page both = {.after = "a", .before = "b", .limit = 10};
    memset(&list, 0x5a, sizeof list);
    int rc = resend_domains_list(client, &both, &list);
    CHECK(rc == -1);
    CHECK(fake_request_count(f) == 0);
    CHECK(list.items == NULL);
    CHECK(list.count == 0);
    const resend_error *err = resend_last_error(client);
    CHECK(err != NULL && err->status == 0);
    CHECK(err->message != NULL && strcmp(err->message, "after and before cannot be combined") == 0);

    resend_page page = {.after = "cursor-1", .limit = 0};
    rc = resend_domains_list(client, &page, &list);
    CHECK(rc == 0);
    CHECK(fake_request_count(f) == 1);
    CHECK(fake_method(f) != NULL && strcmp(fake_method(f), "GET") == 0);
    CHECK(fake_path(f) != NULL && strcmp(fake_path(f), "/domains?after=cursor-1") == 0);
    CHECK(header_eq(f, "Authorization", "Bearer re_test"));
    CHECK(list.has_more == 1);
    CHECK(list.count == 2);
    CHECK(list.items != NULL);
    CHECK(list.items[0].id != NULL && strcmp(list.items[0].id, id) == 0);
    CHECK(list.items[0].name != NULL && strcmp(list.items[0].name, "example.com") == 0);
    CHECK(list.items[0].status != NULL && strcmp(list.items[0].status, "verified") == 0);
    CHECK(list.items[0].region != NULL && strcmp(list.items[0].region, "eu-west-1") == 0);
    CHECK(list.items[0].created_at != NULL &&
          strcmp(list.items[0].created_at, "2023-04-26 20:21:26.347412+00") == 0);
    CHECK(list.items[0].open_tracking == 0);
    CHECK(list.items[0].click_tracking == 1);
    CHECK(list.items[0].tracking_subdomain == NULL);
    CHECK(list.items[0].sending != NULL && strcmp(list.items[0].sending, "enabled") == 0);
    CHECK(list.items[0].receiving != NULL && strcmp(list.items[0].receiving, "disabled") == 0);
    CHECK(list.items[0].records == NULL);
    CHECK(list.items[0].record_count == 0);
    CHECK(list.items[1].id != NULL && strcmp(list.items[1].id, "d2") == 0);
    CHECK(list.items[1].name != NULL && strcmp(list.items[1].name, "other.example") == 0);
    CHECK(list.items[1].status != NULL && strcmp(list.items[1].status, "pending") == 0);
    CHECK(list.items[1].open_tracking == 0);
    CHECK(list.items[1].click_tracking == 0);
    CHECK(list.items[1].sending == NULL);
    CHECK(list.items[1].receiving == NULL);
    CHECK(list.items[1].tracking_subdomain == NULL);
    CHECK(list.items[1].records == NULL);
    resend_domain_list_free(&list);
    CHECK(list.items == NULL);
    CHECK(list.count == 0);
    resend_domain_list_free(&list);
    resend_domain_list_free(NULL);
    resend_free(client);
    fake_stop(f);

    f = fake_start(&exchange, 1);
    CHECK(f != NULL);
    if (!f)
        return;
    client = client_on(f);
    CHECK(client != NULL);
    if (!client) {
        fake_stop(f);
        return;
    }
    resend_page limited = {.before = "cursor-2", .limit = 5};
    rc = resend_domains_list(client, &limited, &list);
    CHECK(rc == 0);
    CHECK(fake_path(f) != NULL && strcmp(fake_path(f), "/domains?before=cursor-2&limit=5") == 0);
    CHECK(list.has_more == 1);
    CHECK(list.count == 2);
    resend_domain_list_free(&list);
    resend_free(client);
    fake_stop(f);

    static const char empty_body[] = "{\"has_more\":false,\"data\":[]}";
    fake_exchange empty_ex = {.status = 200, .body = empty_body};
    f = fake_start(&empty_ex, 1);
    CHECK(f != NULL);
    if (!f)
        return;
    client = client_on(f);
    CHECK(client != NULL);
    if (!client) {
        fake_stop(f);
        return;
    }
    rc = resend_domains_list(client, NULL, &list);
    CHECK(rc == 0);
    CHECK(fake_path(f) != NULL && strcmp(fake_path(f), "/domains") == 0);
    CHECK(list.count == 0);
    CHECK(list.items == NULL);
    CHECK(list.has_more == 0);
    resend_domain_list_free(&list);
    resend_free(client);
    fake_stop(f);

    f = fake_start(&empty_ex, 1);
    CHECK(f != NULL);
    if (!f)
        return;
    client = client_on(f);
    CHECK(client != NULL);
    if (!client) {
        fake_stop(f);
        return;
    }
    resend_page encoded = {.after = "a b&c=d"};
    rc = resend_domains_list(client, &encoded, &list);
    CHECK(rc == 0);
    CHECK(fake_path(f) != NULL && strcmp(fake_path(f), "/domains?after=a%20b%26c%3Dd") == 0);
    resend_domain_list_free(&list);
    resend_free(client);
    fake_stop(f);

    static const fake_exchange bad[] = {
        {.status = 200, .body = "not-json"},
        {.status = 200, .body = "{\"has_more\":truefoo,\"data\":[]}"},
        {.status = 200, .body = "{\"has_more\":false,\"data\":nullfoo}"},
        {.status = 200, .body = "{\"has_more\":false,\"data\":[{\"name\":\"example.com\"}]}"},
        {.status = 200, .body = "{\"has_more\":false,\"data\":[{\"id\":\"x\"},]}"},
        {.status = 200, .body = "{\"has_more\":false,\"data\":[1]}"},
        {.status = 200, .body = "{\"has_more\":false,\"data\":[{\"id\":\"x\",\"region\":true}]}"},
        {.status = 200, .body = "{\"has_more\":false,\"data\":null}"},
        {.status = 200, .body = "{\"has_more\":true}"},
        {.status = 404, .body = "{\"name\":\"not_found\",\"message\":\"Domain not found\"}"},
    };
    f = fake_start(bad, sizeof bad / sizeof bad[0]);
    CHECK(f != NULL);
    if (!f)
        return;
    client = client_on(f);
    CHECK(client != NULL);
    if (!client) {
        fake_stop(f);
        return;
    }
    for (size_t i = 0; i < 7; i++) {
        memset(&list, 0x5a, sizeof list);
        rc = resend_domains_list(client, NULL, &list);
        CHECK(rc == -1);
        CHECK(list.items == NULL);
        CHECK(list.count == 0);
        CHECK(list.has_more == 0);
        err = resend_last_error(client);
        CHECK(err != NULL && err->status == 200);
        CHECK(err->message != NULL && err->message[0] != '\0');
        resend_domain_list_free(&list);
    }
    CHECK(resend_domains_list(client, NULL, &list) == 0);
    CHECK(list.count == 0);
    CHECK(list.items == NULL);
    CHECK(list.has_more == 0);
    resend_domain_list_free(&list);
    CHECK(resend_domains_list(client, NULL, &list) == 0);
    CHECK(list.count == 0);
    CHECK(list.has_more == 1);
    resend_domain_list_free(&list);
    memset(&list, 0x5a, sizeof list);
    rc = resend_domains_list(client, NULL, &list);
    CHECK(rc == -1);
    CHECK(list.items == NULL);
    err = resend_last_error(client);
    CHECK(err != NULL && err->status == 404);
    CHECK(err->name != NULL && strcmp(err->name, "not_found") == 0);
    CHECK(err->message != NULL && strcmp(err->message, "Domain not found") == 0);
    resend_domain_list_free(&list);
    CHECK(fake_request_count(f) == (int)(sizeof bad / sizeof bad[0]));
    resend_free(client);
    fake_stop(f);
}

/* POST /domains. NULL capabilities omit the object. NULL fields inside are left out. */
static void test_domains_create(void) {
    static const char created[] =
        "{\"object\":\"domain\","
        "\"id\":\"d91cd9bd-1176-453e-8fc1-35364d380206\","
        "\"name\":\"example.com\",\"status\":\"not_started\","
        "\"created_at\":\"2023-04-26 20:21:26.347412+00\","
        "\"region\":\"us-east-1\",\"open_tracking\":false,\"click_tracking\":false,"
        "\"tracking_subdomain\":\"links\","
        "\"capabilities\":{\"sending\":\"enabled\",\"receiving\":\"disabled\"},"
        "\"records\":["
        "{\"record\":\"SPF\",\"name\":\"send\",\"type\":\"MX\",\"ttl\":\"Auto\","
        "\"status\":\"not_started\",\"value\":\"feedback-smtp.us-east-1.amazonses.com\","
        "\"priority\":10},"
        "{\"record\":\"DKIM\",\"name\":\"resend._domainkey\",\"type\":\"TXT\","
        "\"ttl\":\"Auto\",\"status\":\"not_started\",\"value\":\"p=MIGfMA0\"}"
        "]}";
    fake_exchange ok = {.status = 201, .body = created};
    fake *f = fake_start(&ok, 1);
    CHECK(f != NULL);
    if (!f)
        return;
    resend *client = client_on(f);
    CHECK(client != NULL);
    if (!client) {
        fake_stop(f);
        return;
    }

    resend_domain domain = {0};
    CHECK(resend_domains_create(NULL, NULL, &domain) == -1);
    CHECK(resend_domains_create(client, NULL, &domain) == -1);
    CHECK(resend_domains_create(client, &(resend_domain_create){.name = "example.com"}, NULL) == -1);
    CHECK(fake_request_count(f) == 0);

    resend_domain_create missing = {0};
    memset(&domain, 0x5a, sizeof domain);
    int rc = resend_domains_create(client, &missing, &domain);
    CHECK(rc == -1);
    CHECK(fake_request_count(f) == 0);
    CHECK(domain.id == NULL);
    CHECK(domain.records == NULL);
    const resend_error *err = resend_last_error(client);
    CHECK(err != NULL && err->status == 0);
    CHECK(err->message != NULL && strcmp(err->message, "missing domain name") == 0);

    resend_domain_create blank = {.name = ""};
    rc = resend_domains_create(client, &blank, &domain);
    CHECK(rc == -1);
    CHECK(fake_request_count(f) == 0);

    resend_domain_create name_only = {.name = "example.com"};
    rc = resend_domains_create(client, &name_only, &domain);
    CHECK(rc == 0);
    CHECK(fake_request_count(f) == 1);
    CHECK(fake_method(f) != NULL && strcmp(fake_method(f), "POST") == 0);
    CHECK(fake_path(f) != NULL && strcmp(fake_path(f), "/domains") == 0);
    CHECK(body_eq(f, "{\"name\":\"example.com\"}"));
    CHECK(domain.id != NULL && strcmp(domain.id, "d91cd9bd-1176-453e-8fc1-35364d380206") == 0);
    CHECK(domain.name != NULL && strcmp(domain.name, "example.com") == 0);
    CHECK(domain.status != NULL && strcmp(domain.status, "not_started") == 0);
    CHECK(domain.region != NULL && strcmp(domain.region, "us-east-1") == 0);
    CHECK(domain.created_at != NULL &&
          strcmp(domain.created_at, "2023-04-26 20:21:26.347412+00") == 0);
    CHECK(domain.tracking_subdomain != NULL && strcmp(domain.tracking_subdomain, "links") == 0);
    CHECK(domain.sending != NULL && strcmp(domain.sending, "enabled") == 0);
    CHECK(domain.receiving != NULL && strcmp(domain.receiving, "disabled") == 0);
    CHECK(domain.records != NULL && domain.record_count == 2);
    CHECK(domain.records != NULL && domain.record_count > 0 && domain.records[0].record != NULL &&
          strcmp(domain.records[0].record, "SPF") == 0);
    CHECK(domain.records != NULL && domain.record_count > 0 && domain.records[0].priority == 10);
    CHECK(domain.records != NULL && domain.record_count > 1 && domain.records[1].record != NULL &&
          strcmp(domain.records[1].record, "DKIM") == 0);
    CHECK(domain.records != NULL && domain.record_count > 1 && domain.records[1].priority == -1);
    CHECK(domain.records != NULL && domain.record_count > 1 && domain.records[1].value != NULL &&
          strcmp(domain.records[1].value, "p=MIGfMA0") == 0);
    resend_domain_free(&domain);
    resend_free(client);
    fake_stop(f);

    static const char id_only[] = "{\"id\":\"d1\"}";
    fake_exchange id_ok = {.status = 201, .body = id_only};

    f = fake_start(&id_ok, 1);
    CHECK(f != NULL);
    if (!f)
        return;
    client = client_on(f);
    CHECK(client != NULL);
    if (!client) {
        fake_stop(f);
        return;
    }
    resend_capabilities sending = {.sending = "enabled"};
    resend_domain_create with_sending = {.name = "example.com", .capabilities = &sending};
    rc = resend_domains_create(client, &with_sending, &domain);
    CHECK(rc == 0);
    CHECK(body_eq(f, "{\"name\":\"example.com\",\"capabilities\":{\"sending\":\"enabled\"}}"));
    CHECK(domain.id != NULL && strcmp(domain.id, "d1") == 0);
    CHECK(domain.records == NULL);
    resend_domain_free(&domain);
    resend_free(client);
    fake_stop(f);

    f = fake_start(&id_ok, 1);
    CHECK(f != NULL);
    if (!f)
        return;
    client = client_on(f);
    CHECK(client != NULL);
    if (!client) {
        fake_stop(f);
        return;
    }
    resend_capabilities bare = {0};
    resend_domain_create with_bare = {.name = "example.com", .capabilities = &bare};
    rc = resend_domains_create(client, &with_bare, &domain);
    CHECK(rc == 0);
    CHECK(body_eq(f, "{\"name\":\"example.com\",\"capabilities\":{}}"));
    resend_domain_free(&domain);
    resend_free(client);
    fake_stop(f);

    f = fake_start(&id_ok, 1);
    CHECK(f != NULL);
    if (!f)
        return;
    client = client_on(f);
    CHECK(client != NULL);
    if (!client) {
        fake_stop(f);
        return;
    }
    resend_capabilities both = {.sending = "disabled", .receiving = "enabled"};
    resend_domain_create with_both = {.name = "example.com", .capabilities = &both};
    rc = resend_domains_create(client, &with_both, &domain);
    CHECK(rc == 0);
    CHECK(body_eq(f, "{\"name\":\"example.com\",\"capabilities\":{\"sending\":\"disabled\","
                     "\"receiving\":\"enabled\"}}"));
    resend_domain_free(&domain);
    resend_free(client);
    fake_stop(f);

    f = fake_start(&id_ok, 1);
    CHECK(f != NULL);
    if (!f)
        return;
    client = client_on(f);
    CHECK(client != NULL);
    if (!client) {
        fake_stop(f);
        return;
    }
    int open_on = 1;
    int click_off = 0;
    resend_capabilities full_caps = {.sending = "enabled"};
    resend_domain_create full = {
        .name = "example.com",
        .region = "eu-west-1",
        .custom_return_path = "bnc",
        .tls = "enforced",
        .tracking_subdomain = "links",
        .open_tracking = &open_on,
        .click_tracking = &click_off,
        .capabilities = &full_caps,
    };
    rc = resend_domains_create(client, &full, &domain);
    CHECK(rc == 0);
    CHECK(body_eq(f, "{\"name\":\"example.com\",\"region\":\"eu-west-1\","
                     "\"custom_return_path\":\"bnc\",\"tls\":\"enforced\","
                     "\"tracking_subdomain\":\"links\",\"open_tracking\":true,"
                     "\"click_tracking\":false,\"capabilities\":{\"sending\":\"enabled\"}}"));
    resend_domain_free(&domain);
    resend_free(client);
    fake_stop(f);

    f = fake_start(&id_ok, 1);
    CHECK(f != NULL);
    if (!f)
        return;
    client = client_on(f);
    CHECK(client != NULL);
    if (!client) {
        fake_stop(f);
        return;
    }
    resend_domain_create quoted = {.name = "a\"b"};
    rc = resend_domains_create(client, &quoted, &domain);
    CHECK(rc == 0);
    CHECK(body_eq(f, "{\"name\":\"a\\\"b\"}"));
    resend_domain_free(&domain);
    resend_free(client);
    fake_stop(f);

    static const fake_exchange bad[] = {
        {.status = 201, .body = "not-json"},
        {.status = 201, .body = "{\"name\":\"example.com\"}"},
        {.status = 201, .body = "{\"id\":\"d1\",\"records\":[{\"ttl\":1}]}"},
        {.status = 422, .body = "{\"name\":\"validation_error\",\"message\":\"Invalid name\"}"},
    };
    f = fake_start(bad, sizeof bad / sizeof bad[0]);
    CHECK(f != NULL);
    if (!f)
        return;
    client = client_on(f);
    CHECK(client != NULL);
    if (!client) {
        fake_stop(f);
        return;
    }
    for (size_t i = 0; i < 3; i++) {
        memset(&domain, 0x5a, sizeof domain);
        rc = resend_domains_create(client, &name_only, &domain);
        CHECK(rc == -1);
        CHECK(domain.id == NULL);
        CHECK(domain.records == NULL);
        err = resend_last_error(client);
        CHECK(err != NULL && err->status == 201);
        CHECK(err->message != NULL && err->message[0] != '\0');
        resend_domain_free(&domain);
    }
    memset(&domain, 0x5a, sizeof domain);
    rc = resend_domains_create(client, &name_only, &domain);
    CHECK(rc == -1);
    CHECK(domain.id == NULL);
    err = resend_last_error(client);
    CHECK(err != NULL && err->status == 422);
    CHECK(err->name != NULL && strcmp(err->name, "validation_error") == 0);
    CHECK(err->message != NULL && strcmp(err->message, "Invalid name") == 0);
    resend_domain_free(&domain);
    CHECK(fake_request_count(f) == (int)(sizeof bad / sizeof bad[0]));
    resend_free(client);
    fake_stop(f);
}

/* PATCH /domains/{id}. NULL fields are omitted. Receiving is not defaulted. */
static void test_domains_update(void) {
    static const char id[] = "d91cd9bd-1176-453e-8fc1-35364d380206";
    fake_exchange ok = {.status = 200, .body = "{\"object\":\"domain\",\"id\":\"d1\"}"};
    fake *f = fake_start(&ok, 1);
    CHECK(f != NULL);
    if (!f)
        return;
    resend *client = client_on(f);
    CHECK(client != NULL);
    if (!client) {
        fake_stop(f);
        return;
    }

    CHECK(resend_domains_update(client, id, NULL) == -1);
    CHECK(fake_request_count(f) == 0);
    CHECK(resend_domains_update(client, "", &(resend_domain_update){0}) == -1);
    CHECK(fake_request_count(f) == 0);

    resend_domain_update empty = {0};
    int rc = resend_domains_update(client, id, &empty);
    CHECK(rc == -1);
    CHECK(fake_request_count(f) == 0);
    const resend_error *err = resend_last_error(client);
    CHECK(err != NULL && err->status == 0);
    CHECK(err->message != NULL && strcmp(err->message, "domain update needs a field") == 0);

    int open_on = 1;
    int click_off = 0;
    resend_capabilities caps = {.sending = "enabled", .receiving = "disabled"};
    resend_domain_update full = {
        .tls = "enforced",
        .tracking_subdomain = "links",
        .open_tracking = &open_on,
        .click_tracking = &click_off,
        .capabilities = &caps,
    };
    rc = resend_domains_update(client, id, &full);
    CHECK(rc == 0);
    CHECK(fake_request_count(f) == 1);
    CHECK(fake_method(f) != NULL && strcmp(fake_method(f), "PATCH") == 0);
    CHECK(fake_path(f) != NULL &&
          strcmp(fake_path(f), "/domains/d91cd9bd-1176-453e-8fc1-35364d380206") == 0);
    CHECK(body_eq(f, "{\"tls\":\"enforced\",\"tracking_subdomain\":\"links\","
                     "\"open_tracking\":true,\"click_tracking\":false,"
                     "\"capabilities\":{\"sending\":\"enabled\",\"receiving\":\"disabled\"}}"));
    resend_free(client);
    fake_stop(f);

    f = fake_start(&ok, 1);
    CHECK(f != NULL);
    if (!f)
        return;
    client = client_on(f);
    CHECK(client != NULL);
    if (!client) {
        fake_stop(f);
        return;
    }
    int open_off = 0;
    resend_domain_update only_open = {.open_tracking = &open_off};
    rc = resend_domains_update(client, id, &only_open);
    CHECK(rc == 0);
    CHECK(body_eq(f, "{\"open_tracking\":false}"));
    resend_free(client);
    fake_stop(f);

    f = fake_start(&ok, 1);
    CHECK(f != NULL);
    if (!f)
        return;
    client = client_on(f);
    CHECK(client != NULL);
    if (!client) {
        fake_stop(f);
        return;
    }
    resend_capabilities sending = {.sending = "enabled"};
    resend_domain_update only_sending = {.capabilities = &sending};
    rc = resend_domains_update(client, id, &only_sending);
    CHECK(rc == 0);
    CHECK(body_eq(f, "{\"capabilities\":{\"sending\":\"enabled\"}}"));
    resend_free(client);
    fake_stop(f);

    f = fake_start(&ok, 1);
    CHECK(f != NULL);
    if (!f)
        return;
    client = client_on(f);
    CHECK(client != NULL);
    if (!client) {
        fake_stop(f);
        return;
    }
    resend_capabilities blank_caps = {0};
    resend_domain_update caps_only = {.capabilities = &blank_caps};
    rc = resend_domains_update(client, id, &caps_only);
    CHECK(rc == 0);
    CHECK(body_eq(f, "{\"capabilities\":{}}"));
    resend_free(client);
    fake_stop(f);

    f = fake_start(&ok, 1);
    CHECK(f != NULL);
    if (!f)
        return;
    client = client_on(f);
    CHECK(client != NULL);
    if (!client) {
        fake_stop(f);
        return;
    }
    resend_domain_update tls = {.tls = "opportunistic"};
    rc = resend_domains_update(client, "a/b c\n?", &tls);
    CHECK(rc == 0);
    CHECK(fake_method(f) != NULL && strcmp(fake_method(f), "PATCH") == 0);
    CHECK(fake_path(f) != NULL && strcmp(fake_path(f), "/domains/a%2Fb%20c%0A%3F") == 0);
    CHECK(body_eq(f, "{\"tls\":\"opportunistic\"}"));
    resend_free(client);
    fake_stop(f);

    fake_exchange missing = {
        .status = 404,
        .body = "{\"name\":\"not_found\",\"message\":\"Domain not found\"}",
    };
    f = fake_start(&missing, 1);
    CHECK(f != NULL);
    if (!f)
        return;
    client = client_on(f);
    CHECK(client != NULL);
    if (!client) {
        fake_stop(f);
        return;
    }
    rc = resend_domains_update(client, id, &tls);
    CHECK(rc == -1);
    err = resend_last_error(client);
    CHECK(err != NULL && err->status == 404);
    CHECK(err->name != NULL && strcmp(err->name, "not_found") == 0);
    CHECK(err->message != NULL && strcmp(err->message, "Domain not found") == 0);
    resend_free(client);
    fake_stop(f);
}

static void test_domains_verify_remove(void) {
    static const char id[] = "d91cd9bd-1176-453e-8fc1-35364d380206";
    fake_exchange ok = {.status = 200, .body = "{\"object\":\"domain\",\"id\":\"d1\"}"};
    fake *f = fake_start(&ok, 1);
    CHECK(f != NULL);
    if (!f)
        return;
    resend *client = client_on(f);
    CHECK(client != NULL);
    if (!client) {
        fake_stop(f);
        return;
    }

    CHECK(resend_domains_verify(NULL, id) == -1);
    CHECK(resend_domains_verify(client, "") == -1);
    CHECK(resend_domains_verify(client, NULL) == -1);
    CHECK(fake_request_count(f) == 0);

    int rc = resend_domains_verify(client, id);
    CHECK(rc == 0);
    CHECK(fake_request_count(f) == 1);
    CHECK(fake_method(f) != NULL && strcmp(fake_method(f), "POST") == 0);
    CHECK(fake_path(f) != NULL &&
          strcmp(fake_path(f), "/domains/d91cd9bd-1176-453e-8fc1-35364d380206/verify") == 0);
    CHECK(fake_body(f) == NULL || fake_body(f)[0] == '\0');
    resend_free(client);
    fake_stop(f);

    f = fake_start(&ok, 1);
    CHECK(f != NULL);
    if (!f)
        return;
    client = client_on(f);
    CHECK(client != NULL);
    if (!client) {
        fake_stop(f);
        return;
    }
    rc = resend_domains_verify(client, "a/b c\n?");
    CHECK(rc == 0);
    CHECK(fake_path(f) != NULL && strcmp(fake_path(f), "/domains/a%2Fb%20c%0A%3F/verify") == 0);
    resend_free(client);
    fake_stop(f);

    fake_exchange missing = {
        .status = 404,
        .body = "{\"name\":\"not_found\",\"message\":\"Domain not found\"}",
    };
    f = fake_start(&missing, 1);
    CHECK(f != NULL);
    if (!f)
        return;
    client = client_on(f);
    CHECK(client != NULL);
    if (!client) {
        fake_stop(f);
        return;
    }
    rc = resend_domains_verify(client, id);
    CHECK(rc == -1);
    const resend_error *err = resend_last_error(client);
    CHECK(err != NULL && err->status == 404);
    CHECK(err->name != NULL && strcmp(err->name, "not_found") == 0);
    resend_free(client);
    fake_stop(f);

    fake_exchange removed = {
        .status = 200,
        .body = "{\"object\":\"domain\",\"id\":\"d1\",\"deleted\":true}",
    };
    f = fake_start(&removed, 1);
    CHECK(f != NULL);
    if (!f)
        return;
    client = client_on(f);
    CHECK(client != NULL);
    if (!client) {
        fake_stop(f);
        return;
    }
    CHECK(resend_domains_remove(client, "") == -1);
    CHECK(resend_domains_remove(client, NULL) == -1);
    CHECK(fake_request_count(f) == 0);
    rc = resend_domains_remove(client, id);
    CHECK(rc == 0);
    CHECK(fake_method(f) != NULL && strcmp(fake_method(f), "DELETE") == 0);
    CHECK(fake_path(f) != NULL &&
          strcmp(fake_path(f), "/domains/d91cd9bd-1176-453e-8fc1-35364d380206") == 0);
    CHECK(fake_body(f) == NULL || fake_body(f)[0] == '\0');
    resend_free(client);
    fake_stop(f);

    f = fake_start(&missing, 1);
    CHECK(f != NULL);
    if (!f)
        return;
    client = client_on(f);
    CHECK(client != NULL);
    if (!client) {
        fake_stop(f);
        return;
    }
    rc = resend_domains_remove(client, id);
    CHECK(rc == -1);
    err = resend_last_error(client);
    CHECK(err != NULL && err->status == 404);
    CHECK(err->message != NULL && strcmp(err->message, "Domain not found") == 0);
    resend_free(client);
    fake_stop(f);
}

/* POST /api-keys. token is set. name and created_at stay NULL. */
static void test_api_keys_create(void) {
    static const char created[] =
        "{\"id\":\"key_1\",\"token\":\"re_123\",\"name\":\"should-stay-null\","
        "\"created_at\":\"2023-10-06 23:47:56.678+00\"}";
    fake_exchange ok = {.status = 201, .body = created};
    fake *f = fake_start(&ok, 1);
    CHECK(f != NULL);
    if (!f)
        return;
    resend *client = client_on(f);
    CHECK(client != NULL);
    if (!client) {
        fake_stop(f);
        return;
    }

    resend_api_key key = {0};
    CHECK(resend_api_keys_create(NULL, NULL, &key) == -1);
    CHECK(resend_api_keys_create(client, NULL, &key) == -1);
    CHECK(resend_api_keys_create(client, &(resend_api_key_create){.name = "prod"}, NULL) == -1);
    CHECK(fake_request_count(f) == 0);

    resend_api_key_create missing = {0};
    memset(&key, 0x5a, sizeof key);
    int rc = resend_api_keys_create(client, &missing, &key);
    CHECK(rc == -1);
    CHECK(fake_request_count(f) == 0);
    CHECK(key.id == NULL);
    CHECK(key.token == NULL);
    const resend_error *err = resend_last_error(client);
    CHECK(err != NULL && err->status == 0);
    CHECK(err->message != NULL && strcmp(err->message, "missing api key name") == 0);

    resend_api_key_create bad_perm = {.name = "prod", .permission = "admin"};
    rc = resend_api_keys_create(client, &bad_perm, &key);
    CHECK(rc == -1);
    CHECK(fake_request_count(f) == 0);
    err = resend_last_error(client);
    CHECK(err != NULL && err->message != NULL &&
          strcmp(err->message, "permission must be full_access or sending_access") == 0);

    resend_api_key_create full_with_domain = {
        .name = "prod",
        .permission = "full_access",
        .domain_id = "d1",
    };
    rc = resend_api_keys_create(client, &full_with_domain, &key);
    CHECK(rc == -1);
    CHECK(fake_request_count(f) == 0);
    err = resend_last_error(client);
    CHECK(err != NULL && err->message != NULL &&
          strcmp(err->message, "domain_id requires sending_access") == 0);

    resend_api_key_create domain_only = {.name = "prod", .domain_id = "d1"};
    rc = resend_api_keys_create(client, &domain_only, &key);
    CHECK(rc == -1);
    CHECK(fake_request_count(f) == 0);

    resend_api_key_create full = {.name = "prod", .permission = "full_access"};
    rc = resend_api_keys_create(client, &full, &key);
    CHECK(rc == 0);
    CHECK(fake_request_count(f) == 1);
    CHECK(fake_method(f) != NULL && strcmp(fake_method(f), "POST") == 0);
    CHECK(fake_path(f) != NULL && strcmp(fake_path(f), "/api-keys") == 0);
    CHECK(body_eq(f, "{\"name\":\"prod\",\"permission\":\"full_access\"}"));
    CHECK(key.id != NULL && strcmp(key.id, "key_1") == 0);
    CHECK(key.token != NULL && strcmp(key.token, "re_123") == 0);
    CHECK(key.name == NULL);
    CHECK(key.created_at == NULL);
    CHECK(key.last_used_at == NULL);
    resend_api_key_free(&key);
    CHECK(key.token == NULL);
    resend_api_key_free(&key);
    resend_api_key_free(NULL);
    resend_free(client);
    fake_stop(f);

    static const char token_only[] = "{\"id\":\"key_2\",\"token\":\"re_send\"}";
    fake_exchange token_ok = {.status = 201, .body = token_only};

    f = fake_start(&token_ok, 1);
    CHECK(f != NULL);
    if (!f)
        return;
    client = client_on(f);
    CHECK(client != NULL);
    if (!client) {
        fake_stop(f);
        return;
    }
    resend_api_key_create sending = {
        .name = "send",
        .permission = "sending_access",
        .domain_id = "d_123",
    };
    rc = resend_api_keys_create(client, &sending, &key);
    CHECK(rc == 0);
    CHECK(body_eq(f, "{\"name\":\"send\",\"permission\":\"sending_access\",\"domain_id\":\"d_123\"}"));
    CHECK(key.token != NULL && strcmp(key.token, "re_send") == 0);
    CHECK(key.name == NULL);
    resend_api_key_free(&key);
    resend_free(client);
    fake_stop(f);

    f = fake_start(&token_ok, 1);
    CHECK(f != NULL);
    if (!f)
        return;
    client = client_on(f);
    CHECK(client != NULL);
    if (!client) {
        fake_stop(f);
        return;
    }
    resend_api_key_create sending_any = {.name = "send", .permission = "sending_access"};
    rc = resend_api_keys_create(client, &sending_any, &key);
    CHECK(rc == 0);
    CHECK(body_eq(f, "{\"name\":\"send\",\"permission\":\"sending_access\"}"));
    resend_api_key_free(&key);
    resend_free(client);
    fake_stop(f);

    f = fake_start(&token_ok, 1);
    CHECK(f != NULL);
    if (!f)
        return;
    client = client_on(f);
    CHECK(client != NULL);
    if (!client) {
        fake_stop(f);
        return;
    }
    resend_api_key_create name_only = {.name = "prod"};
    rc = resend_api_keys_create(client, &name_only, &key);
    CHECK(rc == 0);
    CHECK(body_eq(f, "{\"name\":\"prod\"}"));
    resend_api_key_free(&key);
    resend_free(client);
    fake_stop(f);

    static const fake_exchange bad[] = {
        {.status = 201, .body = "not-json"},
        {.status = 201, .body = "{\"id\":\"key_1\",\"name\":\"prod\"}"},
        {.status = 201, .body = "{\"token\":\"re_123\"}"},
        {.status = 422, .body = "{\"name\":\"validation_error\",\"message\":\"Invalid name\"}"},
    };
    f = fake_start(bad, sizeof bad / sizeof bad[0]);
    CHECK(f != NULL);
    if (!f)
        return;
    client = client_on(f);
    CHECK(client != NULL);
    if (!client) {
        fake_stop(f);
        return;
    }
    for (size_t i = 0; i < 3; i++) {
        memset(&key, 0x5a, sizeof key);
        rc = resend_api_keys_create(client, &name_only, &key);
        CHECK(rc == -1);
        CHECK(key.id == NULL);
        CHECK(key.token == NULL);
        CHECK(key.name == NULL);
        err = resend_last_error(client);
        CHECK(err != NULL && err->status == 201);
        CHECK(err->message != NULL && err->message[0] != '\0');
        resend_api_key_free(&key);
    }
    rc = resend_api_keys_create(client, &name_only, &key);
    CHECK(rc == -1);
    CHECK(key.token == NULL);
    err = resend_last_error(client);
    CHECK(err != NULL && err->status == 422);
    CHECK(err->name != NULL && strcmp(err->name, "validation_error") == 0);
    resend_api_key_free(&key);
    CHECK(fake_request_count(f) == 4);
    resend_free(client);
    fake_stop(f);
}

/* GET /api-keys. Page helper, has_more, last_used_at. token stays NULL. */
static void test_api_keys_list(void) {
    static const char body[] =
        "{\"object\":\"list\",\"has_more\":true,\"data\":["
        "{\"id\":\"key_1\",\"name\":\"prod\","
        "\"created_at\":\"2023-10-06 23:47:56.678+00\","
        "\"last_used_at\":\"2024-01-02 03:04:05.678+00\","
        "\"token\":\"re_should_not_copy\"},"
        "{\"id\":\"key_2\",\"name\":\"send\","
        "\"created_at\":\"2023-11-01 00:00:00.000+00\","
        "\"last_used_at\":null,\"token\":1}"
        "]}";
    fake_exchange exchange = {.status = 200, .body = body};
    fake *f = fake_start(&exchange, 1);
    CHECK(f != NULL);
    if (!f)
        return;
    resend *client = client_on(f);
    CHECK(client != NULL);
    if (!client) {
        fake_stop(f);
        return;
    }

    resend_api_key_list list = {0};
    resend_page both = {.after = "a", .before = "b"};
    memset(&list, 0x5a, sizeof list);
    int rc = resend_api_keys_list(client, &both, &list);
    CHECK(rc == -1);
    CHECK(fake_request_count(f) == 0);
    CHECK(list.items == NULL);
    CHECK(list.count == 0);
    const resend_error *err = resend_last_error(client);
    CHECK(err != NULL && err->message != NULL &&
          strcmp(err->message, "after and before cannot be combined") == 0);

    resend_page page = {.after = "cursor-1", .limit = 0};
    rc = resend_api_keys_list(client, &page, &list);
    CHECK(rc == 0);
    CHECK(fake_method(f) != NULL && strcmp(fake_method(f), "GET") == 0);
    CHECK(fake_path(f) != NULL && strcmp(fake_path(f), "/api-keys?after=cursor-1") == 0);
    CHECK(list.has_more == 1);
    CHECK(list.count == 2);
    CHECK(list.items != NULL);
    CHECK(list.items[0].id != NULL && strcmp(list.items[0].id, "key_1") == 0);
    CHECK(list.items[0].name != NULL && strcmp(list.items[0].name, "prod") == 0);
    CHECK(list.items[0].created_at != NULL &&
          strcmp(list.items[0].created_at, "2023-10-06 23:47:56.678+00") == 0);
    CHECK(list.items[0].last_used_at != NULL &&
          strcmp(list.items[0].last_used_at, "2024-01-02 03:04:05.678+00") == 0);
    CHECK(list.items[0].token == NULL);
    CHECK(list.items[1].id != NULL && strcmp(list.items[1].id, "key_2") == 0);
    CHECK(list.items[1].name != NULL && strcmp(list.items[1].name, "send") == 0);
    CHECK(list.items[1].last_used_at == NULL);
    CHECK(list.items[1].token == NULL);
    resend_api_key_list_free(&list);
    resend_api_key_list_free(&list);
    resend_api_key_list_free(NULL);
    resend_free(client);
    fake_stop(f);

    f = fake_start(&exchange, 1);
    CHECK(f != NULL);
    if (!f)
        return;
    client = client_on(f);
    CHECK(client != NULL);
    if (!client) {
        fake_stop(f);
        return;
    }
    resend_page limited = {.before = "cursor-2", .limit = 5};
    rc = resend_api_keys_list(client, &limited, &list);
    CHECK(rc == 0);
    CHECK(fake_path(f) != NULL && strcmp(fake_path(f), "/api-keys?before=cursor-2&limit=5") == 0);
    resend_api_key_list_free(&list);
    resend_free(client);
    fake_stop(f);

    static const char empty_body[] = "{\"has_more\":false,\"data\":[]}";
    fake_exchange empty_ex = {.status = 200, .body = empty_body};
    f = fake_start(&empty_ex, 1);
    CHECK(f != NULL);
    if (!f)
        return;
    client = client_on(f);
    CHECK(client != NULL);
    if (!client) {
        fake_stop(f);
        return;
    }
    rc = resend_api_keys_list(client, NULL, &list);
    CHECK(rc == 0);
    CHECK(fake_path(f) != NULL && strcmp(fake_path(f), "/api-keys") == 0);
    CHECK(list.count == 0);
    CHECK(list.has_more == 0);
    CHECK(list.items == NULL);
    resend_api_key_list_free(&list);
    resend_free(client);
    fake_stop(f);

    static const fake_exchange bad[] = {
        {.status = 200, .body = "not-json"},
        {.status = 200, .body = "{\"has_more\":false,\"data\":[{\"name\":\"prod\"}]}"},
        {.status = 200, .body = "{\"has_more\":false,\"data\":[{\"id\":\"k\",\"last_used_at\":1}]}"},
        {.status = 200, .body = "{\"has_more\":false,\"data\":null}"},
        {.status = 404, .body = "{\"name\":\"not_found\",\"message\":\"API key not found\"}"},
    };
    f = fake_start(bad, sizeof bad / sizeof bad[0]);
    CHECK(f != NULL);
    if (!f)
        return;
    client = client_on(f);
    CHECK(client != NULL);
    if (!client) {
        fake_stop(f);
        return;
    }
    for (size_t i = 0; i < 3; i++) {
        memset(&list, 0x5a, sizeof list);
        rc = resend_api_keys_list(client, NULL, &list);
        CHECK(rc == -1);
        CHECK(list.items == NULL);
        CHECK(list.count == 0);
        err = resend_last_error(client);
        CHECK(err != NULL && err->status == 200);
        resend_api_key_list_free(&list);
    }
    CHECK(resend_api_keys_list(client, NULL, &list) == 0);
    CHECK(list.count == 0);
    CHECK(list.has_more == 0);
    resend_api_key_list_free(&list);
    rc = resend_api_keys_list(client, NULL, &list);
    CHECK(rc == -1);
    err = resend_last_error(client);
    CHECK(err != NULL && err->status == 404);
    CHECK(err->message != NULL && strcmp(err->message, "API key not found") == 0);
    resend_api_key_list_free(&list);
    CHECK(fake_request_count(f) == (int)(sizeof bad / sizeof bad[0]));
    resend_free(client);
    fake_stop(f);
}

static void test_api_keys_remove(void) {
    static const char id[] = "key_1";
    fake_exchange ok = {
        .status = 200,
        .body = "{\"object\":\"api_key\",\"id\":\"key_1\",\"deleted\":true}",
    };
    fake *f = fake_start(&ok, 1);
    CHECK(f != NULL);
    if (!f)
        return;
    resend *client = client_on(f);
    CHECK(client != NULL);
    if (!client) {
        fake_stop(f);
        return;
    }

    CHECK(resend_api_keys_remove(NULL, id) == -1);
    CHECK(resend_api_keys_remove(client, "") == -1);
    CHECK(resend_api_keys_remove(client, NULL) == -1);
    CHECK(fake_request_count(f) == 0);

    int rc = resend_api_keys_remove(client, id);
    CHECK(rc == 0);
    CHECK(fake_request_count(f) == 1);
    CHECK(fake_method(f) != NULL && strcmp(fake_method(f), "DELETE") == 0);
    CHECK(fake_path(f) != NULL && strcmp(fake_path(f), "/api-keys/key_1") == 0);
    CHECK(fake_body(f) == NULL || fake_body(f)[0] == '\0');
    resend_free(client);
    fake_stop(f);

    f = fake_start(&ok, 1);
    CHECK(f != NULL);
    if (!f)
        return;
    client = client_on(f);
    CHECK(client != NULL);
    if (!client) {
        fake_stop(f);
        return;
    }
    rc = resend_api_keys_remove(client, "a/b c\n?");
    CHECK(rc == 0);
    CHECK(fake_path(f) != NULL && strcmp(fake_path(f), "/api-keys/a%2Fb%20c%0A%3F") == 0);
    resend_free(client);
    fake_stop(f);

    fake_exchange missing = {
        .status = 404,
        .body = "{\"name\":\"not_found\",\"message\":\"API key not found\"}",
    };
    f = fake_start(&missing, 1);
    CHECK(f != NULL);
    if (!f)
        return;
    client = client_on(f);
    CHECK(client != NULL);
    if (!client) {
        fake_stop(f);
        return;
    }
    rc = resend_api_keys_remove(client, id);
    CHECK(rc == -1);
    const resend_error *err = resend_last_error(client);
    CHECK(err != NULL && err->status == 404);
    CHECK(err->name != NULL && strcmp(err->name, "not_found") == 0);
    CHECK(err->message != NULL && strcmp(err->message, "API key not found") == 0);
    resend_free(client);
    fake_stop(f);
}

static void test_api_keys_update(void) {
    fake_exchange ok = {.status = 200, .body = "{\"object\":\"api_key\",\"id\":\"key_1\"}"};
    fake *f = fake_start(&ok, 1);
    CHECK(f != NULL);
    if (!f)
        return;
    resend *client = client_on(f);
    CHECK(client != NULL);
    if (!client) {
        fake_stop(f);
        return;
    }

    CHECK(resend_api_keys_update(NULL, "key_1", "n") == -1);
    CHECK(resend_api_keys_update(client, NULL, "n") == -1);
    CHECK(resend_api_keys_update(client, "", "n") == -1);
    CHECK(resend_api_keys_update(client, "key_1", NULL) == -1);
    CHECK(resend_api_keys_update(client, "key_1", "") == -1);
    CHECK(fake_request_count(f) == 0);

    CHECK(resend_api_keys_update(client, "key_1", "Prod \"main\"") == 0);
    CHECK(fake_request_count(f) == 1);
    CHECK(fake_method(f) != NULL && strcmp(fake_method(f), "PATCH") == 0);
    CHECK(fake_path(f) != NULL && strcmp(fake_path(f), "/api-keys/key_1") == 0);
    CHECK(body_eq(f, "{\"name\":\"Prod \\\"main\\\"\"}"));
    resend_free(client);
    fake_stop(f);

    fake_exchange missing = {
        .status = 404,
        .body = "{\"name\":\"not_found\",\"message\":\"API key not found\"}",
    };
    f = fake_start(&missing, 1);
    CHECK(f != NULL);
    if (!f)
        return;
    client = client_on(f);
    CHECK(client != NULL);
    if (!client) {
        fake_stop(f);
        return;
    }
    CHECK(resend_api_keys_update(client, "key_1", "n") == -1);
    const resend_error *err = resend_last_error(client);
    CHECK(err != NULL && err->status == 404);
    CHECK(err->name != NULL && strcmp(err->name, "not_found") == 0);
    resend_free(client);
    fake_stop(f);
}

/* GET /logs. Page helper, summary fields. Bodies stay NULL when omitted. */
static void test_logs_list(void) {
    static const char body[] =
        "{\"object\":\"list\",\"has_more\":true,\"data\":["
        "{\"id\":\"37e4414c-5e25-4dbc-a071-43552a4bd53b\","
        "\"created_at\":\"2026-03-30 13:43:54.622865+00\","
        "\"endpoint\":\"/emails\",\"method\":\"POST\",\"response_status\":200,"
        "\"user_agent\":\"resend-node:6.0.3\"},"
        "{\"id\":\"a1b2c3d4-5e6f-7a8b-9c0d-1e2f3a4b5c6d\","
        "\"created_at\":\"2026-03-30 12:15:00.123456+00\","
        "\"endpoint\":\"/emails/4ef9a417-02e9-4d39-ad75-9611e0fcc33c\","
        "\"method\":\"GET\",\"user_agent\":null}"
        "]}";
    fake_exchange exchange = {.status = 200, .body = body};
    fake *f = fake_start(&exchange, 1);
    CHECK(f != NULL);
    if (!f)
        return;
    resend *client = client_on(f);
    CHECK(client != NULL);
    if (!client) {
        fake_stop(f);
        return;
    }

    resend_log_list list = {0};
    resend_page both = {.after = "a", .before = "b"};
    memset(&list, 0x5a, sizeof list);
    int rc = resend_logs_list(client, &both, &list);
    CHECK(rc == -1);
    CHECK(fake_request_count(f) == 0);
    CHECK(list.items == NULL);
    CHECK(list.count == 0);
    const resend_error *err = resend_last_error(client);
    CHECK(err != NULL && err->message != NULL &&
          strcmp(err->message, "after and before cannot be combined") == 0);

    CHECK(resend_logs_list(NULL, NULL, &list) == -1);
    CHECK(resend_logs_list(client, NULL, NULL) == -1);
    CHECK(fake_request_count(f) == 0);

    resend_page page = {.after = "cursor-1", .limit = 0};
    rc = resend_logs_list(client, &page, &list);
    CHECK(rc == 0);
    CHECK(fake_method(f) != NULL && strcmp(fake_method(f), "GET") == 0);
    CHECK(fake_path(f) != NULL && strcmp(fake_path(f), "/logs?after=cursor-1") == 0);
    CHECK(list.has_more == 1);
    CHECK(list.count == 2);
    CHECK(list.items != NULL);
    CHECK(list.items[0].id != NULL &&
          strcmp(list.items[0].id, "37e4414c-5e25-4dbc-a071-43552a4bd53b") == 0);
    CHECK(list.items[0].created_at != NULL &&
          strcmp(list.items[0].created_at, "2026-03-30 13:43:54.622865+00") == 0);
    CHECK(list.items[0].endpoint != NULL && strcmp(list.items[0].endpoint, "/emails") == 0);
    CHECK(list.items[0].method != NULL && strcmp(list.items[0].method, "POST") == 0);
    CHECK(list.items[0].response_status == 200);
    CHECK(list.items[0].user_agent != NULL &&
          strcmp(list.items[0].user_agent, "resend-node:6.0.3") == 0);
    CHECK(list.items[0].request_body == NULL);
    CHECK(list.items[0].response_body == NULL);
    CHECK(list.items[1].id != NULL &&
          strcmp(list.items[1].id, "a1b2c3d4-5e6f-7a8b-9c0d-1e2f3a4b5c6d") == 0);
    CHECK(list.items[1].user_agent == NULL);
    CHECK(list.items[1].response_status == 0);
    CHECK(list.items[1].request_body == NULL);
    CHECK(list.items[1].response_body == NULL);
    resend_log_list_free(&list);
    resend_log_list_free(&list);
    resend_log_list_free(NULL);
    resend_free(client);
    fake_stop(f);

    f = fake_start(&exchange, 1);
    CHECK(f != NULL);
    if (!f)
        return;
    client = client_on(f);
    CHECK(client != NULL);
    if (!client) {
        fake_stop(f);
        return;
    }
    resend_page limited = {.before = "cursor-2", .limit = 5};
    rc = resend_logs_list(client, &limited, &list);
    CHECK(rc == 0);
    CHECK(fake_path(f) != NULL && strcmp(fake_path(f), "/logs?before=cursor-2&limit=5") == 0);
    resend_log_list_free(&list);
    resend_free(client);
    fake_stop(f);

    static const char empty_body[] = "{\"has_more\":false,\"data\":[]}";
    fake_exchange empty_ex = {.status = 200, .body = empty_body};
    f = fake_start(&empty_ex, 1);
    CHECK(f != NULL);
    if (!f)
        return;
    client = client_on(f);
    CHECK(client != NULL);
    if (!client) {
        fake_stop(f);
        return;
    }
    rc = resend_logs_list(client, NULL, &list);
    CHECK(rc == 0);
    CHECK(fake_path(f) != NULL && strcmp(fake_path(f), "/logs") == 0);
    CHECK(list.count == 0);
    CHECK(list.has_more == 0);
    CHECK(list.items == NULL);
    resend_log_list_free(&list);
    resend_free(client);
    fake_stop(f);

    static const fake_exchange bad[] = {
        {.status = 200, .body = "not-json"},
        {.status = 200, .body = "{\"has_more\":false,\"data\":[{\"endpoint\":\"/emails\"}]}"},
        {.status = 200, .body = "{\"has_more\":false,\"data\":[{\"id\":\"k\",\"response_status\":\"200\"}]}"},
        {.status = 200, .body = "{\"has_more\":false,\"data\":null}"},
        {.status = 404, .body = "{\"name\":\"not_found\",\"message\":\"Log not found\"}"},
    };
    f = fake_start(bad, sizeof bad / sizeof bad[0]);
    CHECK(f != NULL);
    if (!f)
        return;
    client = client_on(f);
    CHECK(client != NULL);
    if (!client) {
        fake_stop(f);
        return;
    }
    for (size_t i = 0; i < 3; i++) {
        memset(&list, 0x5a, sizeof list);
        rc = resend_logs_list(client, NULL, &list);
        CHECK(rc == -1);
        CHECK(list.items == NULL);
        CHECK(list.count == 0);
        err = resend_last_error(client);
        CHECK(err != NULL && err->status == 200);
        resend_log_list_free(&list);
    }
    CHECK(resend_logs_list(client, NULL, &list) == 0);
    CHECK(list.count == 0);
    CHECK(list.has_more == 0);
    resend_log_list_free(&list);
    rc = resend_logs_list(client, NULL, &list);
    CHECK(rc == -1);
    err = resend_last_error(client);
    CHECK(err != NULL && err->status == 404);
    CHECK(err->message != NULL && strcmp(err->message, "Log not found") == 0);
    resend_log_list_free(&list);
    CHECK(fake_request_count(f) == (int)(sizeof bad / sizeof bad[0]));
    resend_free(client);
    fake_stop(f);
}

/* GET /logs/{id}. Raw request_body and response_body. null stays NULL. */
static void test_logs_get(void) {
    static const char id[] = "37e4414c-5e25-4dbc-a071-43552a4bd53b";
    static const char body[] =
        "{\"object\":\"log\",\"id\":\"37e4414c-5e25-4dbc-a071-43552a4bd53b\","
        "\"created_at\":\"2026-03-30 13:43:54.622865+00\","
        "\"endpoint\":\"/emails\",\"method\":\"POST\",\"response_status\":200,"
        "\"user_agent\":\"resend-node:6.0.3\","
        "\"request_body\":{\"from\":\"Acme <onboarding@resend.dev>\","
        "\"to\":[\"delivered@resend.dev\"],\"subject\":\"Hello World\"},"
        "\"response_body\":{\"id\":\"4ef9a417-02e9-4d39-ad75-9611e0fcc33c\"}}";
    fake_exchange ok = {.status = 200, .body = body};
    fake *f = fake_start(&ok, 1);
    CHECK(f != NULL);
    if (!f)
        return;
    resend *client = client_on(f);
    CHECK(client != NULL);
    if (!client) {
        fake_stop(f);
        return;
    }

    resend_log log = {0};
    CHECK(resend_logs_get(NULL, id, &log) == -1);
    CHECK(resend_logs_get(client, NULL, &log) == -1);
    CHECK(resend_logs_get(client, "", &log) == -1);
    CHECK(resend_logs_get(client, id, NULL) == -1);
    CHECK(fake_request_count(f) == 0);

    memset(&log, 0x5a, sizeof log);
    int rc = resend_logs_get(client, id, &log);
    CHECK(rc == 0);
    CHECK(fake_method(f) != NULL && strcmp(fake_method(f), "GET") == 0);
    CHECK(fake_path(f) != NULL &&
          strcmp(fake_path(f), "/logs/37e4414c-5e25-4dbc-a071-43552a4bd53b") == 0);
    CHECK(log.id != NULL && strcmp(log.id, id) == 0);
    CHECK(log.endpoint != NULL && strcmp(log.endpoint, "/emails") == 0);
    CHECK(log.method != NULL && strcmp(log.method, "POST") == 0);
    CHECK(log.response_status == 200);
    CHECK(log.user_agent != NULL && strcmp(log.user_agent, "resend-node:6.0.3") == 0);
    CHECK(log.request_body != NULL &&
          strcmp(log.request_body, "{\"from\":\"Acme <onboarding@resend.dev>\","
                                   "\"to\":[\"delivered@resend.dev\"],\"subject\":\"Hello World\"}") ==
              0);
    CHECK(log.response_body != NULL &&
          strcmp(log.response_body, "{\"id\":\"4ef9a417-02e9-4d39-ad75-9611e0fcc33c\"}") == 0);
    resend_log_free(&log);
    CHECK(log.id == NULL);
    resend_log_free(&log);
    resend_log_free(NULL);
    resend_free(client);
    fake_stop(f);

    static const char null_bodies[] =
        "{\"id\":\"37e4414c-5e25-4dbc-a071-43552a4bd53b\","
        "\"request_body\":null,\"response_body\":null}";
    fake_exchange null_ex = {.status = 200, .body = null_bodies};
    f = fake_start(&null_ex, 1);
    CHECK(f != NULL);
    if (!f)
        return;
    client = client_on(f);
    CHECK(client != NULL);
    if (!client) {
        fake_stop(f);
        return;
    }
    rc = resend_logs_get(client, id, &log);
    CHECK(rc == 0);
    CHECK(log.request_body == NULL);
    CHECK(log.response_body == NULL);
    resend_log_free(&log);
    resend_free(client);
    fake_stop(f);

    static const char omitted[] = "{\"id\":\"37e4414c-5e25-4dbc-a071-43552a4bd53b\"}";
    fake_exchange omit_ex = {.status = 200, .body = omitted};
    f = fake_start(&omit_ex, 1);
    CHECK(f != NULL);
    if (!f)
        return;
    client = client_on(f);
    CHECK(client != NULL);
    if (!client) {
        fake_stop(f);
        return;
    }
    rc = resend_logs_get(client, id, &log);
    CHECK(rc == 0);
    CHECK(log.request_body == NULL);
    CHECK(log.response_body == NULL);
    CHECK(log.response_status == 0);
    resend_log_free(&log);
    resend_free(client);
    fake_stop(f);

    static const char raw_values[] =
        "{\"id\":\"37e4414c-5e25-4dbc-a071-43552a4bd53b\","
        "\"request_body\":\"not-an-object\",\"response_body\":[1,true,null]}";
    fake_exchange raw_ex = {.status = 200, .body = raw_values};
    f = fake_start(&raw_ex, 1);
    CHECK(f != NULL);
    if (!f)
        return;
    client = client_on(f);
    CHECK(client != NULL);
    if (!client) {
        fake_stop(f);
        return;
    }
    rc = resend_logs_get(client, id, &log);
    CHECK(rc == 0);
    CHECK(log.request_body != NULL && strcmp(log.request_body, "\"not-an-object\"") == 0);
    CHECK(log.response_body != NULL && strcmp(log.response_body, "[1,true,null]") == 0);
    resend_log_free(&log);
    resend_free(client);
    fake_stop(f);

    f = fake_start(&ok, 1);
    CHECK(f != NULL);
    if (!f)
        return;
    client = client_on(f);
    CHECK(client != NULL);
    if (!client) {
        fake_stop(f);
        return;
    }
    rc = resend_logs_get(client, "a/b c\n?", &log);
    CHECK(rc == 0);
    CHECK(fake_path(f) != NULL && strcmp(fake_path(f), "/logs/a%2Fb%20c%0A%3F") == 0);
    resend_log_free(&log);
    static const char *const dots[] = {".", "..", "%2e", "%2E%2e"};
    for (size_t i = 0; i < sizeof dots / sizeof dots[0]; i++) {
        memset(&log, 0, sizeof log);
        rc = resend_logs_get(client, dots[i], &log);
        CHECK(rc == -1);
        CHECK(log.id == NULL);
        const resend_error *err = resend_last_error(client);
        CHECK(err != NULL && err->status == 0);
        CHECK(err->message != NULL && strcmp(err->message, "id cannot be . or ..") == 0);
    }
    CHECK(fake_request_count(f) == 1);
    resend_free(client);
    fake_stop(f);

    static const fake_exchange bad[] = {
        {.status = 200, .body = "not-json"},
        {.status = 200, .body = "{\"endpoint\":\"/emails\"}"},
        {.status = 200, .body = "{\"id\":\"x\",\"user_agent\":1}"},
        {.status = 404, .body = "{\"name\":\"not_found\",\"message\":\"Log not found\"}"},
    };
    f = fake_start(bad, sizeof bad / sizeof bad[0]);
    CHECK(f != NULL);
    if (!f)
        return;
    client = client_on(f);
    CHECK(client != NULL);
    if (!client) {
        fake_stop(f);
        return;
    }
    for (size_t i = 0; i < 3; i++) {
        memset(&log, 0x5a, sizeof log);
        rc = resend_logs_get(client, id, &log);
        CHECK(rc == -1);
        CHECK(log.id == NULL);
        CHECK(log.request_body == NULL);
        const resend_error *err = resend_last_error(client);
        CHECK(err != NULL && err->status == 200);
        resend_log_free(&log);
    }
    rc = resend_logs_get(client, id, &log);
    CHECK(rc == -1);
    const resend_error *err = resend_last_error(client);
    CHECK(err != NULL && err->status == 404);
    CHECK(err->message != NULL && strcmp(err->message, "Log not found") == 0);
    resend_log_free(&log);
    CHECK(fake_request_count(f) == 4);
    resend_free(client);
    fake_stop(f);
}

/* GET /suppressions. Page helper, optional origin passed through. */
static void test_suppressions_list(void) {
    static const char body[] =
        "{\"object\":\"list\",\"has_more\":true,\"data\":["
        "{\"id\":\"e169aa45-1ecf-4183-9955-b1499d5701d3\","
        "\"email\":\"steve.wozniak@example.com\",\"origin\":\"manual\","
        "\"source_id\":null,\"created_at\":\"2026-10-06 23:47:56.678+00\"},"
        "{\"id\":\"520784e2-887d-4c25-b53c-4ad46ad38100\","
        "\"email\":\"susan.kare@example.com\",\"origin\":\"bounce\","
        "\"source_id\":\"4ef9a417-02e9-4d39-ad75-9611e0fcc33c\","
        "\"created_at\":\"2026-10-07 08:12:03.412+00\"}"
        "]}";
    fake_exchange exchange = {.status = 200, .body = body};
    fake *f = fake_start(&exchange, 1);
    CHECK(f != NULL);
    if (!f)
        return;
    resend *client = client_on(f);
    CHECK(client != NULL);
    if (!client) {
        fake_stop(f);
        return;
    }

    resend_suppression_list list = {0};
    resend_page both = {.after = "a", .before = "b"};
    memset(&list, 0x5a, sizeof list);
    int rc = resend_suppressions_list(client, &both, "bounce", &list);
    CHECK(rc == -1);
    CHECK(fake_request_count(f) == 0);
    CHECK(list.items == NULL);
    CHECK(list.count == 0);
    const resend_error *err = resend_last_error(client);
    CHECK(err != NULL && err->message != NULL &&
          strcmp(err->message, "after and before cannot be combined") == 0);

    CHECK(resend_suppressions_list(NULL, NULL, NULL, &list) == -1);
    CHECK(resend_suppressions_list(client, NULL, NULL, NULL) == -1);
    CHECK(fake_request_count(f) == 0);

    resend_page page = {.after = "cursor-1", .limit = 0};
    rc = resend_suppressions_list(client, &page, "bounce", &list);
    CHECK(rc == 0);
    CHECK(fake_method(f) != NULL && strcmp(fake_method(f), "GET") == 0);
    CHECK(fake_path(f) != NULL &&
          strcmp(fake_path(f), "/suppressions?after=cursor-1&origin=bounce") == 0);
    CHECK(list.has_more == 1);
    CHECK(list.count == 2);
    CHECK(list.items != NULL);
    CHECK(list.items[0].id != NULL &&
          strcmp(list.items[0].id, "e169aa45-1ecf-4183-9955-b1499d5701d3") == 0);
    CHECK(list.items[0].email != NULL &&
          strcmp(list.items[0].email, "steve.wozniak@example.com") == 0);
    CHECK(list.items[0].origin != NULL && strcmp(list.items[0].origin, "manual") == 0);
    CHECK(list.items[0].source_id == NULL);
    CHECK(list.items[0].created_at != NULL &&
          strcmp(list.items[0].created_at, "2026-10-06 23:47:56.678+00") == 0);
    CHECK(list.items[0].deleted == 0);
    CHECK(list.items[1].id != NULL &&
          strcmp(list.items[1].id, "520784e2-887d-4c25-b53c-4ad46ad38100") == 0);
    CHECK(list.items[1].origin != NULL && strcmp(list.items[1].origin, "bounce") == 0);
    CHECK(list.items[1].source_id != NULL &&
          strcmp(list.items[1].source_id, "4ef9a417-02e9-4d39-ad75-9611e0fcc33c") == 0);
    resend_suppression_list_free(&list);
    resend_suppression_list_free(&list);
    resend_suppression_list_free(NULL);
    resend_free(client);
    fake_stop(f);

    f = fake_start(&exchange, 1);
    CHECK(f != NULL);
    if (!f)
        return;
    client = client_on(f);
    CHECK(client != NULL);
    if (!client) {
        fake_stop(f);
        return;
    }
    rc = resend_suppressions_list(client, NULL, "complaint", &list);
    CHECK(rc == 0);
    CHECK(fake_path(f) != NULL && strcmp(fake_path(f), "/suppressions?origin=complaint") == 0);
    resend_suppression_list_free(&list);
    resend_free(client);
    fake_stop(f);

    f = fake_start(&exchange, 1);
    CHECK(f != NULL);
    if (!f)
        return;
    client = client_on(f);
    CHECK(client != NULL);
    if (!client) {
        fake_stop(f);
        return;
    }
    rc = resend_suppressions_list(client, NULL, "a b", &list);
    CHECK(rc == 0);
    CHECK(fake_path(f) != NULL && strcmp(fake_path(f), "/suppressions?origin=a%20b") == 0);
    resend_suppression_list_free(&list);
    resend_free(client);
    fake_stop(f);

    static const char empty_body[] = "{\"has_more\":false,\"data\":[]}";
    fake_exchange empty_ex = {.status = 200, .body = empty_body};
    f = fake_start(&empty_ex, 1);
    CHECK(f != NULL);
    if (!f)
        return;
    client = client_on(f);
    CHECK(client != NULL);
    if (!client) {
        fake_stop(f);
        return;
    }
    rc = resend_suppressions_list(client, NULL, NULL, &list);
    CHECK(rc == 0);
    CHECK(fake_path(f) != NULL && strcmp(fake_path(f), "/suppressions") == 0);
    CHECK(list.count == 0);
    CHECK(list.has_more == 0);
    CHECK(list.items == NULL);
    resend_suppression_list_free(&list);
    resend_free(client);
    fake_stop(f);

    static const fake_exchange bad[] = {
        {.status = 200, .body = "not-json"},
        {.status = 200, .body = "{\"has_more\":false,\"data\":[{\"email\":\"a@b.c\"}]}"},
        {.status = 200, .body = "{\"has_more\":false,\"data\":[{\"id\":\"s\",\"source_id\":1}]}"},
        {.status = 200, .body = "{\"has_more\":false,\"data\":null}"},
        {.status = 404, .body = "{\"name\":\"not_found\",\"message\":\"Suppression not found\"}"},
    };
    f = fake_start(bad, sizeof bad / sizeof bad[0]);
    CHECK(f != NULL);
    if (!f)
        return;
    client = client_on(f);
    CHECK(client != NULL);
    if (!client) {
        fake_stop(f);
        return;
    }
    for (size_t i = 0; i < 3; i++) {
        memset(&list, 0x5a, sizeof list);
        rc = resend_suppressions_list(client, NULL, NULL, &list);
        CHECK(rc == -1);
        CHECK(list.items == NULL);
        CHECK(list.count == 0);
        err = resend_last_error(client);
        CHECK(err != NULL && err->status == 200);
        resend_suppression_list_free(&list);
    }
    CHECK(resend_suppressions_list(client, NULL, NULL, &list) == 0);
    CHECK(list.count == 0);
    CHECK(list.has_more == 0);
    resend_suppression_list_free(&list);
    rc = resend_suppressions_list(client, NULL, NULL, &list);
    CHECK(rc == -1);
    err = resend_last_error(client);
    CHECK(err != NULL && err->status == 404);
    CHECK(err->message != NULL && strcmp(err->message, "Suppression not found") == 0);
    resend_suppression_list_free(&list);
    CHECK(fake_request_count(f) == (int)(sizeof bad / sizeof bad[0]));
    resend_free(client);
    fake_stop(f);
}

static void test_suppressions_get(void) {
    static const char id[] = "e169aa45-1ecf-4183-9955-b1499d5701d3";
    static const char body[] =
        "{\"object\":\"suppression\",\"id\":\"e169aa45-1ecf-4183-9955-b1499d5701d3\","
        "\"email\":\"steve.wozniak@example.com\",\"origin\":\"bounce\","
        "\"source_id\":\"4ef9a417-02e9-4d39-ad75-9611e0fcc33c\","
        "\"created_at\":\"2026-10-06 23:47:56.678+00\"}";
    fake_exchange ok = {.status = 200, .body = body};
    fake *f = fake_start(&ok, 1);
    CHECK(f != NULL);
    if (!f)
        return;
    resend *client = client_on(f);
    CHECK(client != NULL);
    if (!client) {
        fake_stop(f);
        return;
    }

    resend_suppression s = {0};
    CHECK(resend_suppressions_get(NULL, id, &s) == -1);
    CHECK(resend_suppressions_get(client, NULL, &s) == -1);
    CHECK(resend_suppressions_get(client, "", &s) == -1);
    CHECK(resend_suppressions_get(client, id, NULL) == -1);
    CHECK(fake_request_count(f) == 0);

    memset(&s, 0x5a, sizeof s);
    int rc = resend_suppressions_get(client, id, &s);
    CHECK(rc == 0);
    CHECK(fake_method(f) != NULL && strcmp(fake_method(f), "GET") == 0);
    CHECK(fake_path(f) != NULL &&
          strcmp(fake_path(f), "/suppressions/e169aa45-1ecf-4183-9955-b1499d5701d3") == 0);
    CHECK(s.id != NULL && strcmp(s.id, id) == 0);
    CHECK(s.email != NULL && strcmp(s.email, "steve.wozniak@example.com") == 0);
    CHECK(s.origin != NULL && strcmp(s.origin, "bounce") == 0);
    CHECK(s.source_id != NULL && strcmp(s.source_id, "4ef9a417-02e9-4d39-ad75-9611e0fcc33c") == 0);
    CHECK(s.created_at != NULL && strcmp(s.created_at, "2026-10-06 23:47:56.678+00") == 0);
    CHECK(s.deleted == 0);
    resend_suppression_free(&s);
    resend_suppression_free(&s);
    resend_suppression_free(NULL);
    resend_free(client);
    fake_stop(f);

    f = fake_start(&ok, 1);
    CHECK(f != NULL);
    if (!f)
        return;
    client = client_on(f);
    CHECK(client != NULL);
    if (!client) {
        fake_stop(f);
        return;
    }
    rc = resend_suppressions_get(client, "steve.wozniak@example.com", &s);
    CHECK(rc == 0);
    CHECK(fake_path(f) != NULL &&
          strcmp(fake_path(f), "/suppressions/steve.wozniak%40example.com") == 0);
    resend_suppression_free(&s);
    static const char *const dots[] = {".", "..", "%2e"};
    for (size_t i = 0; i < sizeof dots / sizeof dots[0]; i++) {
        memset(&s, 0, sizeof s);
        rc = resend_suppressions_get(client, dots[i], &s);
        CHECK(rc == -1);
        CHECK(s.id == NULL);
        const resend_error *err = resend_last_error(client);
        CHECK(err != NULL && err->status == 0);
        CHECK(err->message != NULL && strcmp(err->message, "id cannot be . or ..") == 0);
    }
    CHECK(fake_request_count(f) == 1);
    resend_free(client);
    fake_stop(f);

    fake_exchange missing = {
        .status = 404,
        .body = "{\"name\":\"not_found\",\"message\":\"Suppression not found\"}",
    };
    f = fake_start(&missing, 1);
    CHECK(f != NULL);
    if (!f)
        return;
    client = client_on(f);
    CHECK(client != NULL);
    if (!client) {
        fake_stop(f);
        return;
    }
    rc = resend_suppressions_get(client, id, &s);
    CHECK(rc == -1);
    const resend_error *err = resend_last_error(client);
    CHECK(err != NULL && err->status == 404);
    CHECK(err->message != NULL && strcmp(err->message, "Suppression not found") == 0);
    resend_suppression_free(&s);
    resend_free(client);
    fake_stop(f);
}

static void test_suppressions_add(void) {
    static const char created[] =
        "{\"object\":\"suppression\",\"id\":\"e169aa45-1ecf-4183-9955-b1499d5701d3\"}";
    fake_exchange ok = {.status = 201, .body = created};
    fake *f = fake_start(&ok, 1);
    CHECK(f != NULL);
    if (!f)
        return;
    resend *client = client_on(f);
    CHECK(client != NULL);
    if (!client) {
        fake_stop(f);
        return;
    }

    resend_suppression s = {0};
    CHECK(resend_suppressions_add(NULL, "a@b.c", &s) == -1);
    CHECK(resend_suppressions_add(client, "a@b.c", NULL) == -1);
    CHECK(fake_request_count(f) == 0);

    memset(&s, 0x5a, sizeof s);
    int rc = resend_suppressions_add(client, NULL, &s);
    CHECK(rc == -1);
    CHECK(fake_request_count(f) == 0);
    CHECK(s.id == NULL);
    const resend_error *err = resend_last_error(client);
    CHECK(err != NULL && err->status == 0);
    CHECK(err->message != NULL && strcmp(err->message, "missing email") == 0);

    rc = resend_suppressions_add(client, "", &s);
    CHECK(rc == -1);
    CHECK(fake_request_count(f) == 0);

    rc = resend_suppressions_add(client, "steve.wozniak@example.com", &s);
    CHECK(rc == 0);
    CHECK(fake_request_count(f) == 1);
    CHECK(fake_method(f) != NULL && strcmp(fake_method(f), "POST") == 0);
    CHECK(fake_path(f) != NULL && strcmp(fake_path(f), "/suppressions") == 0);
    CHECK(body_eq(f, "{\"email\":\"steve.wozniak@example.com\"}"));
    CHECK(s.id != NULL && strcmp(s.id, "e169aa45-1ecf-4183-9955-b1499d5701d3") == 0);
    CHECK(s.email == NULL);
    CHECK(s.origin == NULL);
    CHECK(s.deleted == 0);
    resend_suppression_free(&s);
    resend_free(client);
    fake_stop(f);

    static const char id_only[] = "{\"id\":\"e169aa45-1ecf-4183-9955-b1499d5701d3\"}";
    fake_exchange id_ok = {.status = 201, .body = id_only};
    f = fake_start(&id_ok, 1);
    CHECK(f != NULL);
    if (!f)
        return;
    client = client_on(f);
    CHECK(client != NULL);
    if (!client) {
        fake_stop(f);
        return;
    }
    rc = resend_suppressions_add(client, "steve.wozniak@example.com", &s);
    CHECK(rc == 0);
    CHECK(s.id != NULL && strcmp(s.id, "e169aa45-1ecf-4183-9955-b1499d5701d3") == 0);
    CHECK(s.email == NULL);
    CHECK(s.origin == NULL);
    CHECK(s.source_id == NULL);
    CHECK(s.created_at == NULL);
    CHECK(s.deleted == 0);
    resend_suppression_free(&s);
    resend_free(client);
    fake_stop(f);

    static const fake_exchange bad[] = {
        {.status = 201, .body = "not-json"},
        {.status = 201, .body = "{\"email\":\"a@b.c\"}"},
        {.status = 422, .body = "{\"name\":\"validation_error\",\"message\":\"Invalid email\"}"},
    };
    f = fake_start(bad, sizeof bad / sizeof bad[0]);
    CHECK(f != NULL);
    if (!f)
        return;
    client = client_on(f);
    CHECK(client != NULL);
    if (!client) {
        fake_stop(f);
        return;
    }
    for (size_t i = 0; i < 2; i++) {
        memset(&s, 0x5a, sizeof s);
        rc = resend_suppressions_add(client, "a@b.c", &s);
        CHECK(rc == -1);
        CHECK(s.id == NULL);
        err = resend_last_error(client);
        CHECK(err != NULL && err->status == 201);
        resend_suppression_free(&s);
    }
    rc = resend_suppressions_add(client, "a@b.c", &s);
    CHECK(rc == -1);
    err = resend_last_error(client);
    CHECK(err != NULL && err->status == 422);
    CHECK(err->name != NULL && strcmp(err->name, "validation_error") == 0);
    resend_suppression_free(&s);
    CHECK(fake_request_count(f) == 3);
    resend_free(client);
    fake_stop(f);
}

static void test_suppressions_remove(void) {
    static const char id[] = "e169aa45-1ecf-4183-9955-b1499d5701d3";
    static const char body[] =
        "{\"object\":\"suppression\",\"id\":\"e169aa45-1ecf-4183-9955-b1499d5701d3\","
        "\"deleted\":true}";
    fake_exchange ok = {.status = 200, .body = body};
    fake *f = fake_start(&ok, 1);
    CHECK(f != NULL);
    if (!f)
        return;
    resend *client = client_on(f);
    CHECK(client != NULL);
    if (!client) {
        fake_stop(f);
        return;
    }

    resend_suppression s = {0};
    CHECK(resend_suppressions_remove(NULL, id, &s) == -1);
    CHECK(resend_suppressions_remove(client, NULL, &s) == -1);
    CHECK(resend_suppressions_remove(client, "", &s) == -1);
    CHECK(resend_suppressions_remove(client, id, NULL) == -1);
    CHECK(fake_request_count(f) == 0);

    int rc = resend_suppressions_remove(client, id, &s);
    CHECK(rc == 0);
    CHECK(fake_method(f) != NULL && strcmp(fake_method(f), "DELETE") == 0);
    CHECK(fake_path(f) != NULL &&
          strcmp(fake_path(f), "/suppressions/e169aa45-1ecf-4183-9955-b1499d5701d3") == 0);
    CHECK(fake_body(f) == NULL || fake_body(f)[0] == '\0');
    CHECK(s.id != NULL && strcmp(s.id, id) == 0);
    CHECK(s.deleted == 1);
    resend_suppression_free(&s);
    resend_free(client);
    fake_stop(f);

    f = fake_start(&ok, 1);
    CHECK(f != NULL);
    if (!f)
        return;
    client = client_on(f);
    CHECK(client != NULL);
    if (!client) {
        fake_stop(f);
        return;
    }
    rc = resend_suppressions_remove(client, "steve.wozniak@example.com", &s);
    CHECK(rc == 0);
    CHECK(fake_path(f) != NULL &&
          strcmp(fake_path(f), "/suppressions/steve.wozniak%40example.com") == 0);
    CHECK(s.deleted == 1);
    resend_suppression_free(&s);
    resend_free(client);
    fake_stop(f);

    static const char not_deleted[] =
        "{\"object\":\"suppression\",\"id\":\"e169aa45-1ecf-4183-9955-b1499d5701d3\","
        "\"deleted\":false}";
    fake_exchange still = {.status = 200, .body = not_deleted};
    f = fake_start(&still, 1);
    CHECK(f != NULL);
    if (!f)
        return;
    client = client_on(f);
    CHECK(client != NULL);
    if (!client) {
        fake_stop(f);
        return;
    }
    rc = resend_suppressions_remove(client, id, &s);
    CHECK(rc == 0);
    CHECK(s.id != NULL && strcmp(s.id, id) == 0);
    CHECK(s.deleted == 0);
    resend_suppression_free(&s);
    resend_free(client);
    fake_stop(f);

    fake_exchange missing = {
        .status = 404,
        .body = "{\"name\":\"not_found\",\"message\":\"Suppression not found\"}",
    };
    f = fake_start(&missing, 1);
    CHECK(f != NULL);
    if (!f)
        return;
    client = client_on(f);
    CHECK(client != NULL);
    if (!client) {
        fake_stop(f);
        return;
    }
    rc = resend_suppressions_remove(client, id, &s);
    CHECK(rc == -1);
    const resend_error *err = resend_last_error(client);
    CHECK(err != NULL && err->status == 404);
    CHECK(err->message != NULL && strcmp(err->message, "Suppression not found") == 0);
    resend_suppression_free(&s);
    resend_free(client);
    fake_stop(f);
}

static void test_suppressions_batch(void) {
    static const char added[] =
        "{\"data\":["
        "{\"object\":\"suppression\",\"id\":\"e169aa45-1ecf-4183-9955-b1499d5701d3\"},"
        "{\"object\":\"suppression\",\"id\":\"520784e2-887d-4c25-b53c-4ad46ad38100\"}"
        "]}";
    fake_exchange add_ok = {.status = 201, .body = added};
    fake *f = fake_start(&add_ok, 1);
    CHECK(f != NULL);
    if (!f)
        return;
    resend *client = client_on(f);
    CHECK(client != NULL);
    if (!client) {
        fake_stop(f);
        return;
    }

    const char *const emails[] = {"steve.wozniak@example.com", "susan.kare@example.com"};
    resend_suppression_list list = {0};
    CHECK(resend_suppressions_add_batch(NULL, emails, 2, &list) == -1);
    CHECK(resend_suppressions_add_batch(client, emails, 2, NULL) == -1);

    memset(&list, 0x5a, sizeof list);
    int rc = resend_suppressions_add_batch(client, emails, 0, &list);
    CHECK(rc == -1);
    CHECK(fake_request_count(f) == 0);
    CHECK(list.items == NULL);
    const resend_error *err = resend_last_error(client);
    CHECK(err != NULL && err->message != NULL && strcmp(err->message, "batch count below 1") == 0);

    rc = resend_suppressions_add_batch(client, emails, 101, &list);
    CHECK(rc == -1);
    CHECK(fake_request_count(f) == 0);
    err = resend_last_error(client);
    CHECK(err != NULL && err->message != NULL &&
          strcmp(err->message, "batch count above 100") == 0);

    rc = resend_suppressions_add_batch(client, NULL, 2, &list);
    CHECK(rc == -1);
    CHECK(fake_request_count(f) == 0);
    err = resend_last_error(client);
    CHECK(err != NULL && err->message != NULL &&
          strcmp(err->message, "missing batch emails") == 0);

    rc = resend_suppressions_add_batch(client, emails, 2, &list);
    CHECK(rc == 0);
    CHECK(fake_method(f) != NULL && strcmp(fake_method(f), "POST") == 0);
    CHECK(fake_path(f) != NULL && strcmp(fake_path(f), "/suppressions/batch/add") == 0);
    CHECK(body_eq(f, "{\"emails\":[\"steve.wozniak@example.com\",\"susan.kare@example.com\"]}"));
    CHECK(list.has_more == 0);
    CHECK(list.count == 2);
    CHECK(list.items != NULL);
    CHECK(list.items[0].id != NULL &&
          strcmp(list.items[0].id, "e169aa45-1ecf-4183-9955-b1499d5701d3") == 0);
    CHECK(list.items[0].email == NULL);
    CHECK(list.items[1].id != NULL &&
          strcmp(list.items[1].id, "520784e2-887d-4c25-b53c-4ad46ad38100") == 0);
    resend_suppression_list_free(&list);
    resend_free(client);
    fake_stop(f);

    static const char removed[] =
        "{\"data\":["
        "{\"object\":\"suppression\",\"id\":\"e169aa45-1ecf-4183-9955-b1499d5701d3\","
        "\"deleted\":true}"
        "]}";
    fake_exchange rm_ok = {.status = 200, .body = removed};
    f = fake_start(&rm_ok, 1);
    CHECK(f != NULL);
    if (!f)
        return;
    client = client_on(f);
    CHECK(client != NULL);
    if (!client) {
        fake_stop(f);
        return;
    }

    const char *const ids[] = {"e169aa45-1ecf-4183-9955-b1499d5701d3"};
    resend_suppression_batch_remove both = {
        .emails = emails,
        .email_count = 2,
        .ids = ids,
        .id_count = 1,
    };
    memset(&list, 0x5a, sizeof list);
    rc = resend_suppressions_remove_batch(client, &both, &list);
    CHECK(rc == -1);
    CHECK(fake_request_count(f) == 0);
    CHECK(list.items == NULL);
    err = resend_last_error(client);
    CHECK(err != NULL && err->message != NULL &&
          strcmp(err->message, "emails and ids cannot be combined") == 0);

    resend_suppression_batch_remove empty = {0};
    rc = resend_suppressions_remove_batch(client, &empty, &list);
    CHECK(rc == -1);
    CHECK(fake_request_count(f) == 0);
    err = resend_last_error(client);
    CHECK(err != NULL && err->message != NULL &&
          strcmp(err->message, "missing batch emails or ids") == 0);

    CHECK(resend_suppressions_remove_batch(NULL, &empty, &list) == -1);
    CHECK(resend_suppressions_remove_batch(client, NULL, &list) == -1);
    CHECK(resend_suppressions_remove_batch(client, &empty, NULL) == -1);
    CHECK(fake_request_count(f) == 0);

    const char *const many[101] = {0};
    resend_suppression_batch_remove too_many = {.ids = many, .id_count = 101};
    rc = resend_suppressions_remove_batch(client, &too_many, &list);
    CHECK(rc == -1);
    CHECK(fake_request_count(f) == 0);
    err = resend_last_error(client);
    CHECK(err != NULL && err->message != NULL &&
          strcmp(err->message, "batch count above 100") == 0);

    resend_suppression_batch_remove missing_ids = {.id_count = 1};
    rc = resend_suppressions_remove_batch(client, &missing_ids, &list);
    CHECK(rc == -1);
    CHECK(fake_request_count(f) == 0);
    err = resend_last_error(client);
    CHECK(err != NULL && err->message != NULL && strcmp(err->message, "missing batch ids") == 0);

    resend_suppression_batch_remove by_id = {.ids = ids, .id_count = 1};
    rc = resend_suppressions_remove_batch(client, &by_id, &list);
    CHECK(rc == 0);
    CHECK(fake_method(f) != NULL && strcmp(fake_method(f), "POST") == 0);
    CHECK(fake_path(f) != NULL && strcmp(fake_path(f), "/suppressions/batch/remove") == 0);
    CHECK(body_eq(f, "{\"ids\":[\"e169aa45-1ecf-4183-9955-b1499d5701d3\"]}"));
    CHECK(list.has_more == 0);
    CHECK(list.count == 1);
    CHECK(list.items != NULL);
    CHECK(list.items[0].id != NULL &&
          strcmp(list.items[0].id, "e169aa45-1ecf-4183-9955-b1499d5701d3") == 0);
    CHECK(list.items[0].deleted == 1);
    resend_suppression_list_free(&list);
    resend_free(client);
    fake_stop(f);

    f = fake_start(&rm_ok, 1);
    CHECK(f != NULL);
    if (!f)
        return;
    client = client_on(f);
    CHECK(client != NULL);
    if (!client) {
        fake_stop(f);
        return;
    }
    resend_suppression_batch_remove by_email = {.emails = emails, .email_count = 2};
    rc = resend_suppressions_remove_batch(client, &by_email, &list);
    CHECK(rc == 0);
    CHECK(body_eq(f, "{\"emails\":[\"steve.wozniak@example.com\",\"susan.kare@example.com\"]}"));
    CHECK(list.items[0].deleted == 1);
    resend_suppression_list_free(&list);
    resend_free(client);
    fake_stop(f);
}

/* Constants in src/internal.h. Deadlines are not timed. */
int test_limits(void);

int main(void) {
    failures += test_limits();
    test_send();
    test_set_base_url();
    test_error_body();
    test_error_clear_on_reject();
    test_transport_failure();
    test_body_cap();
    test_redirect_follow();
    test_redirect_other_host();
    test_redirect_drop_http();
    test_location_on_success();
    test_location_on_error();
    test_send_fields();
    test_send_html_only();
    test_send_template();
    test_send_attachments();
    test_send_attachment_both();
    test_send_idempotency_key();
    test_send_idempotency_key_max();
    test_emails_get();
    test_emails_list();
    test_emails_cancel();
    test_emails_update();
    test_emails_send_batch();
    test_emails_batch_validation();
    test_emails_batch_errors();
    test_emails_attachments();
    test_emails_id_path();
    test_emails_list_query();
    test_emails_list_bad();
    test_emails_get_bad();
    test_json_nul();
    test_webhook_verify();
    test_receiving_get();
    test_receiving_body_cap();
    test_receiving_list();
    test_receiving_attachments();
    test_download();
    test_domains_get();
    test_domains_list();
    test_domains_create();
    test_domains_update();
    test_domains_verify_remove();
    test_api_keys_create();
    test_api_keys_list();
    test_api_keys_update();
    test_api_keys_remove();
    test_logs_list();
    test_logs_get();
    test_suppressions_list();
    test_suppressions_get();
    test_suppressions_add();
    test_suppressions_remove();
    test_suppressions_batch();
    if (failures != 0) {
        fprintf(stderr, "%d failure%s\n", failures, failures == 1 ? "" : "s");
        return 1;
    }
    return 0;
}
