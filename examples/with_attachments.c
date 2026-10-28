/*
 * Send one message with a small in-memory attachment (inline content_id)
 * and a remote path attachment.
 */
#include <stdio.h>
#include <stdlib.h>

#include "resend.h"

/* 1x1 PNG for the inline cid: image. */
static const unsigned char pixel[] = {
    0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a, 0x00, 0x00, 0x00, 0x0d, 0x49, 0x48,
    0x44, 0x52, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x08, 0x02, 0x00, 0x00,
    0x00, 0x90, 0x77, 0x53, 0xde, 0x00, 0x00, 0x00, 0x0c, 0x49, 0x44, 0x41, 0x54, 0x08,
    0xd7, 0x63, 0xf8, 0xcf, 0xc0, 0x00, 0x00, 0x00, 0x03, 0x00, 0x01, 0x00, 0x05, 0xfe,
    0xd4, 0xef, 0x00, 0x00, 0x00, 0x00, 0x49, 0x45, 0x4e, 0x44, 0xae, 0x42, 0x60, 0x82,
};

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

    resend_attachment attachments[] = {
        {
            .filename = "pixel.png",
            .content = pixel,
            .content_len = sizeof(pixel),
            .content_type = "image/png",
            .content_id = "pixel",
        },
        {
            .filename = "invoice.pdf",
            .path = "https://raw.githubusercontent.com/resend/resend-go/main/resources/invoice.pdf",
            .content_type = "application/pdf",
        },
    };

    const char *to[] = {"delivered@resend.dev"};
    resend_send req = {
        .from = "Acme <onboarding@resend.dev>",
        .to = to,
        .to_count = 1,
        .subject = "Email with attachments",
        .html = "<p>email with attachments <img src=\"cid:pixel\" alt=\"pixel\"/></p>",
        .text = "email with attachments",
        .attachments = attachments,
        .attachment_count = 2,
    };

    resend_sent sent = {0};
    if (resend_emails_send(c, &req, &sent) != 0) {
        fprintf(stderr, "%s\n", resend_last_error(c)->message);
        resend_free(c);
        return 1;
    }
    printf("Sent email ID: %s\n", sent.id);
    resend_sent_free(&sent);

    resend_free(c);
    return 0;
}
