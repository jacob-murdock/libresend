/*
 * List received emails. If empty, exit 0. Otherwise get the first id
 * (or argv[1]) with html_format cid, list attachments, get each, and
 * download when a download_url is present.
 */
#include <stdio.h>
#include <stdlib.h>

#include "resend.h"

int main(int argc, char **argv)
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

    resend_page page = {.limit = 10};
    resend_received_list list = {0};
    if (resend_receiving_list(c, &page, &list) != 0) {
        fprintf(stderr, "%s\n", resend_last_error(c)->message);
        resend_free(c);
        return 1;
    }
    printf("Received %zu emails, has_more=%d\n", list.count, list.has_more);

    const char *email_id = NULL;
    if (argc > 1 && argv[1][0]) {
        email_id = argv[1];
    } else if (list.count > 0) {
        email_id = list.items[0].id;
    } else {
        printf("No received emails; nothing to get.\n");
        resend_received_list_free(&list);
        resend_free(c);
        return 0;
    }

    resend_received email = {0};
    if (resend_receiving_get(c, email_id, "cid", &email) != 0) {
        fprintf(stderr, "%s\n", resend_last_error(c)->message);
        resend_received_list_free(&list);
        resend_free(c);
        return 1;
    }
    printf("Get: id=%s subject=%s from=%s attachments=%zu html_format=%s\n",
           email.id,
           email.subject ? email.subject : "",
           email.from ? email.from : "",
           email.attachment_count,
           email.html_format ? email.html_format : "");
    resend_received_free(&email);

    resend_received_attachment_list atts = {0};
    if (resend_receiving_attachments_list(c, email_id, NULL, &atts) != 0) {
        fprintf(stderr, "%s\n", resend_last_error(c)->message);
        resend_received_list_free(&list);
        resend_free(c);
        return 1;
    }
    printf("Attachment list: %zu, has_more=%d\n", atts.count, atts.has_more);

    for (size_t i = 0; i < atts.count; i++) {
        resend_received_attachment one = {0};
        if (resend_receiving_attachment_get(c, email_id, atts.items[i].id, &one) != 0) {
            fprintf(stderr, "%s\n", resend_last_error(c)->message);
            resend_received_attachment_list_free(&atts);
            resend_received_list_free(&list);
            resend_free(c);
            return 1;
        }
        printf("  %s (%s)\n",
               one.filename ? one.filename : "",
               one.content_type ? one.content_type : "");
        if (one.download_url) {
            void *bytes = NULL;
            size_t len = 0;
            if (resend_download(c, one.download_url, &bytes, &len) != 0) {
                fprintf(stderr, "%s\n", resend_last_error(c)->message);
                resend_received_attachment_free(&one);
                resend_received_attachment_list_free(&atts);
                resend_received_list_free(&list);
                resend_free(c);
                return 1;
            }
            printf("    downloaded %zu bytes\n", len);
            free(bytes);
        }
        resend_received_attachment_free(&one);
    }

    resend_received_attachment_list_free(&atts);
    resend_received_list_free(&list);
    resend_free(c);
    return 0;
}
