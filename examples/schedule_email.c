/*
 * Schedule a send for "in 1 hour", get that id, then cancel it.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "resend.h"

static void pause_ms(unsigned ms)
{
    struct timespec ts = {
        .tv_sec = ms / 1000,
        .tv_nsec = (long)(ms % 1000) * 1000000L,
    };
    nanosleep(&ts, NULL);
}

/*
 * The id shows up before last_event is "scheduled". Until then it is often
 * "queued", and cancel answers "Email is not scheduled".
 */
static int wait_scheduled(resend *c, const char *id, resend_email *email)
{
    static const unsigned wait_ms[] = {200, 400, 800, 1600, 3000, 3000};
    for (size_t attempt = 0;; attempt++) {
        resend_email_free(email);
        if (resend_emails_get(c, id, email) != 0) {
            const resend_error *e = resend_last_error(c);
            int missing = e->name && strcmp(e->name, "not_found") == 0;
            if (!missing || attempt >= sizeof wait_ms / sizeof wait_ms[0])
                return -1;
        } else if (email->last_event && strcmp(email->last_event, "scheduled") == 0) {
            return 0;
        } else if (!email->last_event || !email->last_event[0] ||
                   strcmp(email->last_event, "queued") == 0) {
            if (attempt >= sizeof wait_ms / sizeof wait_ms[0])
                return -1;
        } else {
            return -1;
        }
        pause_ms(wait_ms[attempt]);
    }
}

/* Cancel can still lose the race with the scheduled state. */
static int cancel_scheduled(resend *c, const char *id, resend_email *email)
{
    static const unsigned wait_ms[] = {200, 400, 800, 1600, 3000};
    for (size_t attempt = 0;; attempt++) {
        resend_email_free(email);
        if (resend_emails_cancel(c, id, email) == 0)
            return 0;
        const resend_error *e = resend_last_error(c);
        if (!e->message || !strstr(e->message, "not scheduled") ||
            attempt >= sizeof wait_ms / sizeof wait_ms[0])
            return -1;
        pause_ms(wait_ms[attempt]);
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
    resend_send req = {
        .from = "Acme <onboarding@resend.dev>",
        .to = to,
        .to_count = 1,
        .subject = "Hello from C (scheduled)",
        .text = "hello world",
        .scheduled_at = "in 1 hour",
    };

    resend_sent sent = {0};
    if (resend_emails_send(c, &req, &sent) != 0) {
        fprintf(stderr, "%s\n", resend_last_error(c)->message);
        resend_free(c);
        return 1;
    }
    printf("Scheduled: %s\n", sent.id);

    resend_email email = {0};
    if (wait_scheduled(c, sent.id, &email) != 0) {
        const resend_error *e = resend_last_error(c);
        if (e->message && e->message[0])
            fprintf(stderr, "%s\n", e->message);
        else
            fprintf(stderr, "last_event=%s\n", email.last_event ? email.last_event : "");
    } else {
        printf("Get: id=%s last_event=%s\n", email.id,
               email.last_event ? email.last_event : "");
    }
    resend_email_free(&email);

    if (cancel_scheduled(c, sent.id, &email) != 0) {
        fprintf(stderr, "%s\n", resend_last_error(c)->message);
        resend_sent_free(&sent);
        resend_free(c);
        return 1;
    }
    printf("Canceled: %s last_event=%s\n", email.id,
           email.last_event ? email.last_event : "");
    resend_email_free(&email);
    resend_sent_free(&sent);

    resend_free(c);
    return 0;
}
