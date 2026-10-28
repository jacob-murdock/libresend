/*
 * Create a full_access API key, print the token once, list, rename, remove.
 */
#include <stdio.h>
#include <stdlib.h>

#include "resend.h"

/* Print the current error, then remove a key this process created. */
static int fail(resend *c, char *api_key_id)
{
    fprintf(stderr, "%s\n", resend_last_error(c)->message);
    if (api_key_id && resend_api_keys_remove(c, api_key_id) != 0)
        fprintf(stderr, "%s\n", resend_last_error(c)->message);
    free(api_key_id);
    resend_free(c);
    return 1;
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

    resend_api_key_create create = {
        .name = "libresend example key",
        .permission = "full_access",
    };

    resend_api_key created = {0};
    if (resend_api_keys_create(c, &create, &created) != 0)
        return fail(c, NULL);
    printf("Created API key id: %s\n", created.id);
    printf("Token: %s\n", created.token ? created.token : "");

    char *id = created.id;
    created.id = NULL;
    resend_api_key_free(&created);

    resend_api_key_list list = {0};
    if (resend_api_keys_list(c, NULL, &list) != 0)
        return fail(c, id);
    printf("You have %zu api keys\n", list.count);
    resend_api_key_list_free(&list);

    if (resend_api_keys_update(c, id, "libresend example key (renamed)") != 0)
        return fail(c, id);
    printf("Renamed API key id: %s\n", id);

    if (resend_api_keys_remove(c, id) != 0) {
        fprintf(stderr, "%s\n", resend_last_error(c)->message);
        free(id);
        resend_free(c);
        return 1;
    }
    printf("Deleted API key id: %s\n", id);
    free(id);

    resend_free(c);
    return 0;
}
