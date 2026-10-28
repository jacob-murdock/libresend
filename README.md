# libresend

Unofficial C client for the [Resend](https://resend.com) API. Not a Resend product, and not affiliated.

[Official SDKs](https://resend.com/docs/sdks) cover Node.js, PHP, Python, Ruby, Go, Java, Rust, and .NET. There is no official C SDK. This library is the part a C program needs: send mail, read what was sent, verify a webhook, read mail Resend received, and manage the domains, API keys, logs, and suppressions that mail depends on.

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

`resend_emails_send_batch` sends up to 100 messages in one call and uses the batch's idempotency key. `validation` is `strict` or `permissive`, sent as `x-batch-validation`. NULL omits that header. Strict is the API default: one invalid message fails the batch. Permissive sends the valid messages, and the result is those ids. `resend_emails_get` and `resend_emails_list` return the stored message, including `message_id` and `last_event`. `resend_emails_update` changes `scheduled_at` on a scheduled send and returns the id. `resend_emails_cancel` cancels a scheduled send and returns that message.

`resend_emails_attachments_list` and `resend_emails_attachment_get` return attachments of a sent email. The type is the same one receiving uses, including `download_url` and `expires_at`. `resend_download` fetches that URL.

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

List results carry the envelope and attachment metadata. Get also fills `html`, `text`, `headers`, `received_for`, `authentication`, and `raw`. `authentication` is the SPF, DKIM, and DMARC result Resend computed. `html_format=data_uri` can be large; get allows 64 MiB, the same cap as download.

Attachments on the email have no download URL. `resend_receiving_attachments_list` and `resend_receiving_attachment_get` return a signed `download_url` and `expires_at`. `resend_download` GETs that HTTPS URL, or `raw.download_url`, and follows redirects that stay on HTTPS. It honors proxy environment variables. The call fails past 64 MiB instead of buffering without limit. A 30-second stall aborts the transfer; there is no total time cap.

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

`resend_domains_create` returns the DNS records to publish. `record` is the purpose (`SPF`, `DKIM`, `Receiving`, `Tracking`, `TrackingCAA`). `type` is the DNS type (`MX`, `TXT`, `CNAME`, `CAA`). Receiving stays off until `capabilities.receiving` is `"enabled"`. A domain list omits records; get and create include them. `resend_domains_verify` starts DNS checks. `resend_domains_update` changes tracking, TLS, and capabilities; pass NULL for a field to leave it. NULL `capabilities` leaves the object out; a zeroed one sends `{}` and the API decides. `region` is `us-east-1` (default), `eu-west-1`, `sa-east-1`, or `ap-northeast-1`. TLS mode and `custom_return_path` are inputs only; get and list can't report them, because the API doesn't return them.

`resend_api_keys_create` takes `full_access` or `sending_access`. A sending key can be limited to one `domain_id`. `token` is present only on create. `resend_api_keys_update` renames a key; the response is only its id.

## Logs

`resend_logs_list` pages through recent API requests. `resend_logs_get` is the same record plus `request_body` and `response_body` as the raw JSON the API stored. A missing `response_status` is 0. `user_agent` is NULL when omitted.

## Suppressions

`resend_suppressions_add` takes an email. List can pass `origin` (`bounce`, `complaint`, `manual`); the client does not check the value. Get and remove take an id or an email. Batch add and batch remove take up to 100 addresses. Remove batch is emails or ids, not both. `source_id` is the email that bounced or complained, and NULL for a manual suppression.

## Calls

Lists take a `resend_page` (`after`, `before`, `limit`). NULL omits pagination. `after` and `before` cannot be combined. `limit` 0 omits the parameter. The API maximum is 100.

| | |
| --- | --- |
| Emails | `resend_emails_send`, `resend_emails_send_batch`, `resend_emails_get`, `resend_emails_list`, `resend_emails_update`, `resend_emails_cancel`, `resend_emails_attachments_list`, `resend_emails_attachment_get` |
| Receiving | `resend_receiving_get`, `resend_receiving_list`, `resend_receiving_attachments_list`, `resend_receiving_attachment_get`, `resend_download` |
| Webhooks | `resend_webhook_verify` |
| Domains | `resend_domains_create`, `resend_domains_get`, `resend_domains_list`, `resend_domains_verify`, `resend_domains_update`, `resend_domains_remove` |
| API keys | `resend_api_keys_create`, `resend_api_keys_list`, `resend_api_keys_update`, `resend_api_keys_remove` |
| Logs | `resend_logs_list`, `resend_logs_get` |
| Suppressions | `resend_suppressions_add`, `resend_suppressions_list`, `resend_suppressions_get`, `resend_suppressions_remove`, `resend_suppressions_add_batch`, `resend_suppressions_remove_batch` |

## Contract

Request pointers belong to the caller for the length of the call. A result belongs to the caller until the matching `*_free`. One client is not shared across threads. `resend_last_error` lasts until the next call on that client. Webhook verify has no client, so it writes `resend_error` itself.

## Scope

The API is larger than this library. What is here is the transactional path. The rest is not started, and pull requests are welcome.

| Area | Status |
| --- | --- |
| Emails, batch, receiving, attachments, webhook verify | Supported |
| Domains, API keys, logs, suppressions | Supported |
| Contacts, segments, topics, contact properties, broadcasts, templates | Not yet |
| Automations, events, Inboxes, share links, domain claims, webhook endpoint management | Not yet |

## License

[MIT](LICENSE). Copyright 2026 Apellus Magnus.
