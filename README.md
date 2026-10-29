# libresend

Unofficial C client for the [Resend](https://resend.com) API. Not a Resend product, and not affiliated. Explore Resend's [official SDK page](https://resend.com/docs/sdks).

A C program can send mail, read what was sent, verify a webhook, and read mail Resend received. It can also manage the domains, API keys, logs, and suppressions that mail depends on. The interface is [`include/resend.h`](include/resend.h). Names follow the [Resend API](https://resend.com/docs/api-reference/introduction) and [resend-openapi](https://github.com/resend/resend-openapi).

## Build

C99, on libcurl and OpenSSL. Tested on Linux. `make install` targets Linux; the library also compiles on macOS and Windows (MinGW). `pkg-config` names: `libcurl`, `openssl`.

libcurl 7.85 or newer is the current baseline. An older libcurl still links.

```bash
make
make check
```

`make` builds `build/libresend.a`. `make check` runs the suite against a loopback fake in the test process. It does not call Resend. It also compiles the public header under `-pedantic` and runs the build, install, and dist scripts.

`make install` copies the static library, the shared library (soname `libresend.so.0`), `include/resend.h`, and `libresend.pc` under `PREFIX` (`/usr/local`). `make uninstall` removes that set.

`make examples` builds the programs in [`examples/`](examples/). It is separate from `make check`.

Releases are source tarballs from `make dist`.

## Send

```c
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
const char *to[] = { "delivered@resend.dev" };
resend_send req = {
    .from = "Acme <onboarding@resend.dev>",
    .to = to,
    .to_count = 1,
    .subject = "Hello",
    .text = "Hello from C.",
    .idempotency_key = "hello-1",
};
resend_sent sent = {0};
if (resend_emails_send(c, &req, &sent) != 0) {
    fprintf(stderr, "%s\n", resend_last_error(c)->message);
    resend_free(c);
    return 1;
}
printf("%s\n", sent.id);
resend_sent_free(&sent);
resend_free(c);
```

A display name is part of the address string, as `Name <email>`. Without a verified domain, send from `onboarding@resend.dev` to `delivered@resend.dev`, `bounced@resend.dev`, `complained@resend.dev`, or `suppressed@resend.dev`. Resend rejects recipients on domains such as `example.com`.

The other programs in [`examples/`](examples/) cover the rest of the library:

- `send_email` sends, then gets and lists.
- `send_batch_email` sends a batch.
- `schedule_email` schedules a send and cancels it.
- `with_attachments` sends an inline attachment and a remote one.
- `receiving` lists received mail, then gets and downloads.
- `domains` creates a domain and prints its DNS records.
- `api_keys` creates a key, lists, and removes it.
- `webhook` verifies a signed body.

## Contract

Request pointers belong to the caller for the length of the call. A result belongs to the caller until the matching `*_free`. One client is not shared across threads. `resend_last_error` lasts until the next call on that client.

Webhook verify takes the raw body and the `svix-id`, `svix-timestamp`, and `svix-signature` headers. It does not use an API key.

Ordinary calls use a 30-second timeout, a 2-second connect timeout, and a 1 MiB body cap. Receiving get, logs get, and download allow more. Those limits are on the declarations in `resend.h`.

## Scope

The API is larger than this library. What is here is the transactional path. The rest is not started, and pull requests are welcome. Sending with a published template id or `topic_id` is supported. Managing contacts, segments, topics, contact properties, broadcasts, and templates is not.

| Area | Status |
| --- | --- |
| Emails, batch, receiving, attachments, webhook verify | Supported |
| Domains, API keys, logs, suppressions | Supported |
| Contacts, segments, topics, contact properties, broadcasts, templates | Not yet |
| Email metrics, automations, events, Inboxes, share links, domain claims, webhook endpoint management | Not yet |

## License

[MIT](LICENSE). Copyright 2026 Apellus Magnus.
