/*
 * Verify a Resend webhook. Reads
 * RESEND_WEBHOOK_SECRET, Svix headers from argv, and the raw body from stdin.
 * Does not use an API key and does not listen on a port.
 *
 * usage: webhook <svix-id> <svix-timestamp> <svix-signature> < body
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "resend.h"

int main(int argc, char **argv)
{
    if (argc < 4) {
        fprintf(stderr, "usage: %s <svix-id> <svix-timestamp> <svix-signature> < body\n",
                argv[0]);
        return 1;
    }

    const char *secret = getenv("RESEND_WEBHOOK_SECRET");
    if (!secret || !secret[0]) {
        fprintf(stderr, "RESEND_WEBHOOK_SECRET is required\n");
        return 1;
    }

    size_t cap = 4096;
    if (cap > RESEND_WEBHOOK_BODY_MAX)
        cap = RESEND_WEBHOOK_BODY_MAX;
    size_t len = 0;
    char *body = malloc(cap);
    if (!body) {
        fprintf(stderr, "out of memory\n");
        return 1;
    }
    for (;;) {
        if (len == RESEND_WEBHOOK_BODY_MAX) {
            char extra;
            size_t n = fread(&extra, 1, 1, stdin);
            if (ferror(stdin))
                break;
            if (n != 0) {
                free(body);
                fprintf(stderr, "webhook body too large\n");
                return 1;
            }
            break;
        }
        if (len == cap) {
            size_t ncap = cap > RESEND_WEBHOOK_BODY_MAX / 2 ? RESEND_WEBHOOK_BODY_MAX : cap * 2;
            char *grown = realloc(body, ncap);
            if (!grown) {
                free(body);
                fprintf(stderr, "out of memory\n");
                return 1;
            }
            body = grown;
            cap = ncap;
        }
        size_t n = fread(body + len, 1, cap - len, stdin);
        len += n;
        if (n == 0)
            break;
    }
    if (ferror(stdin)) {
        free(body);
        fprintf(stderr, "failed to read body\n");
        return 1;
    }

    resend_event event = {0};
    resend_error err = {0};
    if (resend_webhook_verify(secret, argv[1], argv[2], argv[3], body, len, &event,
                              &err) != 0) {
        fprintf(stderr, "%s\n", err.message ? err.message : "verify failed");
        free(body);
        return 1;
    }
    printf("type=%s\n", event.type ? event.type : "");
    if (event.email_id)
        printf("email_id=%s\n", event.email_id);
    if (event.created_at)
        printf("created_at=%s\n", event.created_at);
    resend_event_free(&event);
    free(body);
    return 0;
}
