/*
 * Send a batch of two messages, then the same batch with a batch
 * Idempotency-Key. Message-level keys are not sent.
 */
#include <stdio.h>
#include <stdlib.h>

#include "resend.h"

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
    resend_send messages[] = {
        {
            .from = "Acme <onboarding@resend.dev>",
            .to = to,
            .to_count = 1,
            .subject = "Hello from batch 1",
            .text = "hey",
        },
        {
            .from = "Acme <onboarding@resend.dev>",
            .to = to,
            .to_count = 1,
            .subject = "Hello from batch 2",
            .text = "hellooo",
        },
    };

    resend_batch batch = {
        .messages = messages,
        .count = 2,
    };

    resend_sent_list sent = {0};
    if (resend_emails_send_batch(c, &batch, &sent) != 0) {
        fprintf(stderr, "%s\n", resend_last_error(c)->message);
        resend_free(c);
        return 1;
    }
    printf("Sent without options: %zu ids\n", sent.count);
    for (size_t i = 0; i < sent.count; i++)
        printf("  %s\n", sent.items[i].id);
    resend_sent_list_free(&sent);

    /* Fixed key: same body within 24h returns the first ids; a different body errors. */
    batch.idempotency_key = "68656c6c6f2d776f726c64";
    if (resend_emails_send_batch(c, &batch, &sent) != 0) {
        fprintf(stderr, "%s\n", resend_last_error(c)->message);
        resend_free(c);
        return 1;
    }
    printf("Sent with idempotency key: %zu ids\n", sent.count);
    for (size_t i = 0; i < sent.count; i++)
        printf("  %s\n", sent.items[i].id);
    resend_sent_list_free(&sent);

    resend_free(c);
    return 0;
}
