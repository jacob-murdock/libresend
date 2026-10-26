/*
 * libresend suite. The compiler and main are the runner.
 * CHECK counts misses and keeps going. The process exits non-zero
 * when the count is not zero. This file includes the public header
 * only; the fake is the wire.
 */

#include "resend.h"

#include "fake.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
        resend_set_base_url(client, base);
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

    resend_set_base_url(client, base);
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

    resend_set_base_url(client, "http://127.0.0.1:1");
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

/* Nothing listens on port 1; the call must fail before an HTTP status. */
static void test_transport_failure(void) {
    const char *const to[] = {"you@example.com"};
    resend_send message = smoke_message(to);
    resend_sent sent = {0};

    resend *client = resend_init("re_test");
    CHECK(client != NULL);
    if (!client)
        return;

    resend_set_base_url(client, "http://127.0.0.1:1");
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

    resend_set_base_url(client, base);
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

    resend_set_base_url(client, base);
    int rc = resend_emails_send(client, &message, &sent);
    CHECK(rc == 0);
    CHECK(sent.id != NULL && strcmp(sent.id, id) == 0);
    CHECK(fake_request_count(f) == 2);

    resend_sent_free(&sent);
    resend_free(client);
    fake_stop(f);
}

/* Absolute http:// aimed at this fake. A follow would parse the id. */
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

    resend_set_base_url(client, base);
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

    resend_set_base_url(client, base);
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

    resend_set_base_url(client, base);
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

    resend_set_base_url(client, base);
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

    resend_set_base_url(client, base);
    int rc = resend_emails_send(client, &message, &sent);
    CHECK(rc == -1);
    CHECK(sent.id == NULL);
    CHECK(fake_request_count(f) == 0);
    const resend_error *err = resend_last_error(client);
    CHECK(err != NULL && err->status == 0);
    CHECK(err->message != NULL && err->message[0] != '\0');

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

    resend_set_base_url(client, base);
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

    resend_set_base_url(client, base);
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

int main(void) {
    test_send();
    test_error_body();
    test_transport_failure();
    test_body_cap();
    test_redirect_follow();
    test_redirect_drop_http();
    test_send_fields();
    test_send_html_only();
    test_send_attachments();
    test_send_attachment_both();
    test_send_idempotency_key();
    test_send_idempotency_key_max();
    if (failures != 0) {
        fprintf(stderr, "%d failure%s\n", failures, failures == 1 ? "" : "s");
        return 1;
    }
    return 0;
}
