/*
 * Send an email, send again with an Idempotency-Key, get that id, and list
 * with a page (limit, then after).
 *
 * Recipients are Resend test addresses. example.com is rejected.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "resend.h"

/*
 * POST /emails returns an id before GET /emails/{id} can see it.
 * not_found in that window is retried. Any other error is returned as-is.
 */
static int get_sent(resend *c, const char *id, resend_email *email)
{
    static const unsigned wait_ms[] = {200, 400, 800, 1600, 3000};
    for (size_t attempt = 0;; attempt++) {
        if (resend_emails_get(c, id, email) == 0)
            return 0;
        const resend_error *e = resend_last_error(c);
        if (!e->name || strcmp(e->name, "not_found") != 0 ||
            attempt >= sizeof wait_ms / sizeof wait_ms[0])
            return -1;
        struct timespec ts = {
            .tv_sec = wait_ms[attempt] / 1000,
            .tv_nsec = (long)(wait_ms[attempt] % 1000) * 1000000L,
        };
        nanosleep(&ts, NULL);
    }
}

int main(void)
{
    const char *key = getenv("RESEND_API_KEY");
    if (!key || !key[0]) {
        fprintf(stderr, "RESEND_API_KEY is required\n");
        return 1;
    }

    resend *c = resend_init(key);
    if (!c) {
        fprintf(stderr, "resend_init failed\n");
        return 1;
    }

    const char *to[] = {"delivered@resend.dev"};
    const char *cc[] = {"delivered+cc@resend.dev"};
    const char *bcc[] = {"delivered+bcc@resend.dev"};
    const char *reply_to[] = {"delivered+reply@resend.dev"};

    resend_send req = {
        .from = "Acme <onboarding@resend.dev>",
        .to = to,
        .to_count = 1,
        .cc = cc,
        .cc_count = 1,
        .bcc = bcc,
        .bcc_count = 1,
        .reply_to = reply_to,
        .reply_to_count = 1,
        .subject = "Hello from C",
        .text = "hello world",
    };

    resend_sent sent = {0};
    if (resend_emails_send(c, &req, &sent) != 0) {
        fprintf(stderr, "%s\n", resend_last_error(c)->message);
        resend_free(c);
        return 1;
    }
    printf("Sent basic email: %s\n", sent.id);
    resend_sent_free(&sent);

    /* Fixed key: same body within 24h returns the first id; a different body errors. */
    req.idempotency_key = "unique-idempotency-key";
    if (resend_emails_send(c, &req, &sent) != 0) {
        fprintf(stderr, "%s\n", resend_last_error(c)->message);
        resend_free(c);
        return 1;
    }
    printf("Sent email with idempotency key: %s\n", sent.id);

    resend_email email = {0};
    if (get_sent(c, sent.id, &email) != 0) {
        fprintf(stderr, "%s\n", resend_last_error(c)->message);
        resend_sent_free(&sent);
        resend_free(c);
        return 1;
    }
    printf("Get: id=%s subject=%s last_event=%s\n",
           email.id, email.subject ? email.subject : "",
           email.last_event ? email.last_event : "");
    resend_email_free(&email);
    resend_sent_free(&sent);

    printf("\nListing emails with limit:\n");
    resend_page page = {.limit = 3};
    resend_email_list list = {0};
    if (resend_emails_list(c, &page, &list) != 0) {
        fprintf(stderr, "%s\n", resend_last_error(c)->message);
        resend_free(c);
        return 1;
    }
    printf("Found %zu emails (limited to 3), has_more=%d\n", list.count, list.has_more);

    if (list.has_more && list.count > 0) {
        const char *after = list.items[list.count - 1].id;
        printf("\nFetching next page after %s\n", after);
        page.after = after;
        resend_email_list next = {0};
        if (resend_emails_list(c, &page, &next) != 0) {
            fprintf(stderr, "%s\n", resend_last_error(c)->message);
            resend_email_list_free(&list);
            resend_free(c);
            return 1;
        }
        printf("Found %zu more emails in next page\n", next.count);
        resend_email_list_free(&next);
    }
    resend_email_list_free(&list);

    resend_free(c);
    return 0;
}
