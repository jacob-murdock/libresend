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

/* GET /emails/{id} fills resend_email, including message_id and last_event. */
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

    resend_set_base_url(client, base);
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
        "\"last_event\":\"delivered\"},"
        "{\"id\":\"3a9f8c2b-1e5d-4f8a-9c7b-2d6e5f8a9c7b\","
        "\"message_id\":\"<444-555-666@email.example.com>\","
        "\"to\":[\"user@example.com\"],"
        "\"from\":\"Acme <onboarding@resend.dev>\","
        "\"created_at\":\"2026-04-03 21:45:12.345678+00\","
        "\"subject\":\"Welcome to Acme\","
        "\"bcc\":null,\"cc\":null,\"reply_to\":null,"
        "\"last_event\":\"opened\"}]}";

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
    resend_set_base_url(client, base);

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
    CHECK(list.items[1].id != NULL &&
          strcmp(list.items[1].id, "3a9f8c2b-1e5d-4f8a-9c7b-2d6e5f8a9c7b") == 0);
    CHECK(list.items[1].last_event != NULL && strcmp(list.items[1].last_event, "opened") == 0);

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
    resend_set_base_url(client, base);

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

    resend_set_base_url(client, base);
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
    resend_set_base_url(client, base);

    int rc = resend_emails_send_batch(client, &batch, &sent);
    CHECK(rc == 0);
    CHECK(fake_request_count(f) == 1);
    CHECK(fake_method(f) != NULL && strcmp(fake_method(f), "POST") == 0);
    CHECK(fake_path(f) != NULL && strcmp(fake_path(f), "/emails/batch") == 0);
    CHECK(header_eq(f, "Idempotency-Key", "batch-1"));
    CHECK(fake_body(f) != NULL &&
          strcmp(fake_body(f),
                 "[{\"from\":\"Acme <hello@example.com>\",\"to\":[\"one@example.com\"],"
                 "\"subject\":\"One\",\"text\":\"first\"},"
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
    resend_set_base_url(client, base);

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
        resend_set_base_url(bad_key, base);
        resend_batch one = {.messages = &injected, .count = 1};
        rc = resend_emails_send_batch(bad_key, &one, &sent);
        CHECK(rc == -1);
        CHECK(fake_request_count(f) == 0);
        resend_free(bad_key);
    }

    resend_set_base_url(client, "http://127.0.0.1:1\r\nX: y");
    resend_batch plain = {.messages = &injected, .count = 1};
    rc = resend_emails_send_batch(client, &plain, &sent);
    CHECK(rc == -1);
    CHECK(fake_request_count(f) == 0);
    resend_set_base_url(client, base);

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

static resend *client_on(fake *f) {
    char base[64];
    snprintf(base, sizeof base, "http://127.0.0.1:%d", fake_port(f));
    resend *client = resend_init("re_test");
    if (client)
        resend_set_base_url(client, base);
    return client;
}

/* Id is one path segment. Slash, space, newline, and '?' stay in the segment.
 * ".." is not squashed. Subject escapes cover a BMP character and an emoji pair. */
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

    static const char dotdot_body[] = "{\"id\":\"..\"}";
    fake_exchange dotdot = {.status = 200, .body = dotdot_body};
    f = fake_start(&dotdot, 1);
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
    rc = resend_emails_get(client, "..", &email);
    CHECK(rc == 0);
    CHECK(fake_path(f) != NULL && strcmp(fake_path(f), "/emails/..") == 0);
    CHECK(email.id != NULL && strcmp(email.id, "..") == 0);
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
        {.status = 200, .body = "{\"id\":\"x\",\"subject\":\"hi\\u0000\"}"},
        {.status = 200, .body = "{\"subject\":\"no id\"}"},
        {.status = 200, .body = "{\"id\":\"x\",\"subject\":\"a\nb\"}"},
        {.status = 200, .body = "{\"id\":\"x\",\"to\":[\"a\",]}"},
        {.status = 200, .body = "{\"id\":\"x\",\"html\":truefoo}"},
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
    rc = resend_webhook_verify("MfKQ9r8GKYqrTwjUPD8ILPZIo2LaLaSw", svix_id, ts, "v1,aa", body,
                               strlen(body), &event, &err);
    CHECK(rc == -1);
    CHECK(strcmp(err.message, "invalid secret") == 0);
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
    test_emails_get();
    test_emails_list();
    test_emails_cancel();
    test_emails_send_batch();
    test_emails_id_path();
    test_emails_list_query();
    test_emails_list_bad();
    test_emails_get_bad();
    test_webhook_verify();
    if (failures != 0) {
        fprintf(stderr, "%d failure%s\n", failures, failures == 1 ? "" : "s");
        return 1;
    }
    return 0;
}
