# libresend

Unofficial C client for the [Resend](https://resend.com) API. Not a Resend product, and not affiliated.

[Official SDKs](https://resend.com/docs/sdks) cover Node.js, PHP, Python, Ruby, Go, Java, Rust, and .NET. There is no official C SDK. This library is the part a C program needs: send mail, read what was sent, verify a webhook, read mail Resend received, and manage the domains and API keys that mail depends on.

The interface is [`include/resend.h`](include/resend.h). Names follow the [Resend API](https://resend.com/docs/api-reference/introduction) and [resend-openapi](https://github.com/resend/resend-openapi). Calls land in that order.

## Build

Linux. Depends on libcurl, OpenSSL, and a C11 compiler. `pkg-config` names: `libcurl`, `openssl`.

```bash
make
make check
```

`make` builds `build/libresend.a`. `make check` runs the suite against a loopback fake in the test process. It does not call Resend. `make install` lands with the pkg-config file.

## Send

```c
resend *c = resend_init(getenv("RESEND_API_KEY"));
const char *to[] = { "you@example.com" };
resend_send req = {
    .from = "Acme <hello@example.com>",
    .to = to,
    .to_count = 1,
    .subject = "Hello",
    .text = "Hello from C.",
    .idempotency_key = "hello-1",
};
resend_sent sent = {0};
if (resend_emails_send(c, &req, &sent) != 0) {
    const resend_error *e = resend_last_error(c);
    fprintf(stderr, "%s\n", e->message);
}
printf("%s\n", sent.id);
resend_sent_free(&sent);
resend_free(c);
```

A display name is part of the address string, as `Name <email>`. `to`, `cc`, `bcc`, and `reply_to` are arrays of those strings. `Idempotency-Key` is a request header, at most 256 characters, and it expires after 24 hours. `scheduled_at` is natural language (`in 1 min`) or ISO 8601. An attachment is raw bytes or a `path` URL.

`resend_emails_send_batch` sends up to 100 messages in one call and uses the batch's idempotency key. `resend_emails_get` and `resend_emails_list` return the stored message, including `message_id` and `last_event`. `resend_emails_cancel` cancels a scheduled send and returns that message.

## Receiving

```c
resend_received email = {0};
if (resend_receiving_get(c, email_id, "cid", &email) != 0) {
    const resend_error *e = resend_last_error(c);
    fprintf(stderr, "%s\n", e->message);
}
resend_received_free(&email);
```

`html_format` is `data_uri` or `cid`. `data_uri` inlines images. `cid` keeps `cid:` references that match an attachment `content_id`. A message too large to inline comes back as `cid` either way; the response field says which.

List results carry the envelope and attachment metadata. Get also fills `html`, `text`, `headers`, `received_for`, `authentication`, and `raw`. `authentication` is the SPF, DKIM, and DMARC result Resend computed.

Attachments on the email have no download URL. `resend_receiving_attachments_list` and `resend_receiving_attachment_get` return a signed `download_url` and `expires_at`. `resend_download` GETs that HTTPS URL, or `raw.download_url`, and follows redirects that stay on HTTPS. The call fails past 64 MiB instead of buffering without limit.

## Webhooks

Verification does not use an API key. Pass the raw body and the `svix-id`, `svix-timestamp`, and `svix-signature` headers. Do not re-serialize the JSON first. The signed payload is `id.timestamp.body`. A timestamp more than five minutes from now is rejected. Callers that need once-only delivery record `svix-id` values themselves.

```c
resend_event event = {0};
resend_error err = {0};
if (resend_webhook_verify(secret, svix_id, svix_timestamp, svix_signature,
                          body, body_len, &event, &err) != 0) {
    fprintf(stderr, "%s\n", err.message);
}
/* event.type is "email.received", "email.sent", and the other event names. */
resend_event_free(&event);
```

`email_id` is set when the payload has one. The rest of `data` stays in `data_json`.

## Domains and API keys

`resend_domains_create` returns the DNS records to publish. `record` is the purpose (`SPF`, `DKIM`, `Receiving`, `Tracking`, `TrackingCAA`). `type` is the DNS type (`MX`, `TXT`, `CNAME`, `CAA`). Receiving stays off until `capabilities.receiving` is `"enabled"`. A domain list omits records; get and create include them. `resend_domains_verify` starts DNS checks. `resend_domains_update` changes tracking, TLS, and capabilities; pass NULL for a field to leave it.

`resend_api_keys_create` takes `full_access` or `sending_access`. A sending key can be limited to one `domain_id`. `token` is present only on create.

## Calls

Lists take a `resend_page` (`after`, `before`, `limit`). NULL omits pagination. `after` and `before` cannot be combined. `limit` 0 omits the parameter. The API maximum is 100.

| | |
| --- | --- |
| Emails | `resend_emails_send`, `resend_emails_send_batch`, `resend_emails_get`, `resend_emails_list`, `resend_emails_cancel` |
| Receiving | `resend_receiving_get`, `resend_receiving_list`, `resend_receiving_attachments_list`, `resend_receiving_attachment_get`, `resend_download` |
| Webhooks | `resend_webhook_verify` |
| Domains | `resend_domains_create`, `resend_domains_get`, `resend_domains_list`, `resend_domains_verify`, `resend_domains_update`, `resend_domains_remove` |
| API keys | `resend_api_keys_create`, `resend_api_keys_list`, `resend_api_keys_remove` |

## Contract

Request pointers belong to the caller for the length of the call. A result belongs to the caller until the matching `*_free`. One client is not shared across threads. `resend_last_error` lasts until the next call on that client. Webhook verify has no client, so it writes `resend_error` itself.

## Later

Contacts, segments, broadcasts, templates, topics, and Inboxes. Also later: rescheduling a send, share links, domain claims, webhook endpoint management, and attachments of a sent email.

A daemon that stores received mail is a separate project. It can link this library once send and receiving work.

## License

[MIT](LICENSE). Copyright 2026 Apellus Magnus.
