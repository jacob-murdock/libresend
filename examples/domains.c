/*
 * Create a domain (name from argv[1]), print DNS records, get, update
 * tracking, list, verify, then remove the domain this process created.
 */
#include <stdio.h>
#include <stdlib.h>

#include "resend.h"

/* Print the current error, then remove a domain this process created. */
static int fail(resend *c, char *domain_id)
{
    fprintf(stderr, "%s\n", resend_last_error(c)->message);
    if (domain_id && resend_domains_remove(c, domain_id) != 0)
        fprintf(stderr, "%s\n", resend_last_error(c)->message);
    free(domain_id);
    resend_free(c);
    return 1;
}

int main(int argc, char **argv)
{
    if (argc < 2 || !argv[1][0]) {
        fprintf(stderr, "usage: %s <domain-name>\n", argv[0]);
        return 1;
    }

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

    resend_domain_create create = {
        .name = argv[1],
        .region = "us-east-1",
    };

    resend_domain domain = {0};
    if (resend_domains_create(c, &create, &domain) != 0)
        return fail(c, NULL);
    printf("Created domain id: %s status: %s\n", domain.id, domain.status ? domain.status : "");
    for (size_t i = 0; i < domain.record_count; i++) {
        const resend_dns_record *r = &domain.records[i];
        printf("  %s %s %s %s\n",
               r->record ? r->record : "",
               r->type ? r->type : "",
               r->name ? r->name : "",
               r->value ? r->value : "");
    }

    char *id = domain.id;
    domain.id = NULL; /* keep id across free for later calls */
    resend_domain_free(&domain);

    if (resend_domains_get(c, id, &domain) != 0)
        return fail(c, id);
    printf("Retrieved domain: %s (%s)\n", domain.name ? domain.name : "",
           domain.status ? domain.status : "");
    resend_domain_free(&domain);

    int open_tracking = 1;
    int click_tracking = 1;
    resend_domain_update update = {
        .open_tracking = &open_tracking,
        .click_tracking = &click_tracking,
    };
    if (resend_domains_update(c, id, &update) != 0)
        return fail(c, id);
    printf("Updated tracking on %s\n", id);

    resend_domain_list list = {0};
    if (resend_domains_list(c, NULL, &list) != 0)
        return fail(c, id);
    printf("You have %zu domains\n", list.count);
    resend_domain_list_free(&list);

    if (resend_domains_verify(c, id) != 0)
        return fail(c, id);
    printf("Verify started for %s\n", id);

    if (resend_domains_remove(c, id) != 0) {
        fprintf(stderr, "%s\n", resend_last_error(c)->message);
        free(id);
        resend_free(c);
        return 1;
    }
    printf("Removed domain id: %s\n", id);
    free(id);

    resend_free(c);
    return 0;
}
