/*
 * libresend — unofficial C client for the Resend API.
 *
 * Request pointers stay with the caller for the duration of the call.
 * A result struct is the caller's after a successful return, until the
 * matching *_free. Strings inside a result are freed with that result.
 *
 * resend_last_error() is valid until the next call on the same client.
 * One client is not shared across threads.
 *
 * Webhook verify does not take a client. The body must be the raw bytes
 * Resend signed, not a re-serialized JSON value.
 *
 * Names follow the Resend API. An address is one string, "Name <email>"
 * when it has a display name. NULL omits an optional pointer. An optional
 * bool is a const int * (NULL omits it; otherwise 0 or 1). A list call
 * takes resend_page; NULL means no query. limit 0 omits the parameter.
 * after and before cannot be combined.
 */
#ifndef RESEND_H
#define RESEND_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct resend resend;

typedef struct {
    int status;          /* HTTP status, or 0 when no response arrived */
    const char *name;    /* Resend error name, or NULL */
    const char *message; /* never NULL; empty string if unknown */
} resend_error;

resend *resend_init(const char *api_key);
void resend_free(resend *client);
/* Tests. Production calls api.resend.com. 0 stored the URL. */
int resend_set_base_url(resend *client, const char *base_url);
const resend_error *resend_last_error(const resend *client);

/* ---- shared pieces ----------------------------------------------------- */

typedef struct {
    const char *name;
    const char *value;
} resend_pair;

typedef struct {
    const char *after;  /* NULL omits. Cannot be combined with before. */
    const char *before;
    size_t limit;       /* 0 omits. The API maximum is 100. */
} resend_page;

typedef struct {
    const char *filename;     /* required when content is set */
    const void *content;      /* raw bytes; base64 on the wire */
    size_t content_len;
    const char *path;         /* hosted file URL; alternative to content */
    const char *content_type; /* optional */
    const char *content_id;   /* optional; inline images, referenced as cid: */
} resend_attachment;

/* ---- emails ------------------------------------------------------------ */

typedef struct {
    const char *from; /* "Name <email>" or "email" */
    const char *const *to;
    size_t to_count; /* 1..50 */
    const char *const *cc;
    size_t cc_count;
    const char *const *bcc;
    size_t bcc_count;
    const char *const *reply_to;
    size_t reply_to_count;
    const char *subject;
    const char *html; /* at least one of html or text */
    const char *text;
    const resend_pair *headers;
    size_t header_count;
    const resend_pair *tags;
    size_t tag_count;
    const resend_attachment *attachments; /* content or path, not both */
    size_t attachment_count;
    const char *scheduled_at;    /* optional; natural language or ISO 8601 */
    const char *idempotency_key; /* Idempotency-Key header, max 256; not JSON */
} resend_send;

typedef struct {
    char *id;
} resend_sent;

typedef struct {
    const resend_send *messages;
    size_t count; /* Resend accepts up to 100 */
    const char *idempotency_key; /* this request's header; message keys are ignored */
    const char *validation; /* "strict" or "permissive"; NULL omits x-batch-validation */
} resend_batch;

typedef struct {
    resend_sent *items;
    size_t count;
} resend_sent_list;

typedef struct {
    char *id;
    char *message_id;
    char *from;
    char **to;
    size_t to_count;
    char **cc;
    size_t cc_count;
    char **bcc;
    size_t bcc_count;
    char **reply_to;
    size_t reply_to_count;
    char *subject;
    char *html;
    char *text;
    char *created_at;
    char *last_event;
} resend_email;

typedef struct {
    resend_email *items;
    size_t count;
    int has_more;
} resend_email_list;

/* 0 on success, -1 on failure. Details in resend_last_error. */
int resend_emails_send(resend *client, const resend_send *message, resend_sent *out);
int resend_emails_send_batch(resend *client, const resend_batch *batch, resend_sent_list *out);
int resend_emails_get(resend *client, const char *email_id, resend_email *out);
int resend_emails_list(resend *client, const resend_page *page, resend_email_list *out);
/* Reschedule a scheduled send. scheduled_at is natural language or ISO 8601. Fills id. */
int resend_emails_update(resend *client, const char *email_id, const char *scheduled_at,
                         resend_sent *out);
int resend_emails_cancel(resend *client, const char *email_id, resend_email *out);

void resend_sent_free(resend_sent *sent);
void resend_sent_list_free(resend_sent_list *list);
void resend_email_free(resend_email *email);
void resend_email_list_free(resend_email_list *list);

/* ---- receiving --------------------------------------------------------- */

typedef struct {
    char *id;
    char *filename;
    char *content_type;
    char *content_disposition;
    char *content_id;
    int64_t size;
    char *download_url; /* set by attachment get/list; NULL on an email payload */
    char *expires_at;
} resend_received_attachment;

typedef struct {
    resend_received_attachment *items;
    size_t count;
    int has_more;
} resend_received_attachment_list;

/*
 * SPF, DKIM, and DMARC as Resend computed them. All three are NULL when
 * the API omitted authentication, which includes every list item.
 * Values: "pass", "fail", "gray", "processing_failed", "unknown".
 */
typedef struct {
    char *spf;
    char *dkim;
    char *dmarc;
} resend_authentication;

typedef struct {
    char *name;
    char *value;
} resend_header;

/* raw on a received email. Both NULL when the API omitted it, and on list. */
typedef struct {
    char *download_url;
    char *expires_at;
} resend_raw;

/*
 * List fills id, from, to, cc, bcc, reply_to, subject, message_id,
 * created_at, and attachment metadata. Get also fills html, text,
 * html_format, headers, received_for, authentication, and raw.
 */
typedef struct {
    char *id;
    char *from;
    char **to;
    size_t to_count;
    char **cc;
    size_t cc_count;
    char **bcc;
    size_t bcc_count;
    char **reply_to;
    size_t reply_to_count;
    char **received_for;
    size_t received_for_count;
    char *subject;
    char *message_id;
    char *html;
    char *html_format; /* "data_uri" or "cid"; may be "cid" even if data_uri was requested */
    char *text;
    resend_header *headers;
    size_t header_count;
    char *created_at;
    resend_authentication authentication;
    resend_raw raw;
    resend_received_attachment *attachments;
    size_t attachment_count;
} resend_received;

typedef struct {
    resend_received *items;
    size_t count;
    int has_more;
} resend_received_list;

/* html_format is "data_uri", "cid", or NULL to omit the query. 64 MiB cap. */
int resend_receiving_get(resend *client, const char *email_id, const char *html_format,
                         resend_received *out);
int resend_receiving_list(resend *client, const resend_page *page, resend_received_list *out);
int resend_receiving_attachments_list(resend *client, const char *email_id, const resend_page *page,
                                     resend_received_attachment_list *out);
int resend_receiving_attachment_get(resend *client, const char *email_id, const char *attachment_id,
                                   resend_received_attachment *out);

/*
 * Download raw.download_url, or an attachment download_url.
 * HTTPS only, redirects included. Honors proxy environment variables.
 * Fails past 64 MiB. Aborts after a 30s stall; no total time cap.
 * *out_bytes is malloc'd; the caller frees it.
 */
int resend_download(resend *client, const char *https_url, void **out_bytes, size_t *out_len);

void resend_received_free(resend_received *email);
void resend_received_list_free(resend_received_list *list);
void resend_received_attachment_free(resend_received_attachment *attachment);
void resend_received_attachment_list_free(resend_received_attachment_list *list);

/* ---- webhooks ---------------------------------------------------------- */

typedef struct {
    char *type;        /* "email.received", "email.sent", ... */
    char *created_at;  /* NULL if omitted; the event timestamp, not data.created_at */
    char *email_id;    /* NULL if the event has none */
    char *data_json;   /* NULL if data is omitted; otherwise the "data" object */
} resend_event;

/*
 * secret is the signing secret. A "whsec_" prefix is stripped when present.
 * svix_signature may contain more than one space-separated "v1," value.
 * Rejects a timestamp outside five minutes of now. Callers that need
 * once-only delivery record svix ids themselves.
 * On success, *out is filled and must be passed to resend_event_free.
 */
int resend_webhook_verify(const char *secret, const char *svix_id, const char *svix_timestamp,
                          const char *svix_signature, const void *body, size_t body_len,
                          resend_event *out, resend_error *err);

void resend_event_free(resend_event *event);

/* ---- domains ----------------------------------------------------------- */

typedef struct {
    char *record; /* "SPF", "DKIM", "Receiving", "Tracking", "TrackingCAA" */
    char *name;
    char *type;   /* "MX", "TXT", "CNAME", "CAA" */
    char *value;
    char *status;
    char *ttl;
    int priority; /* -1 when the record has none */
} resend_dns_record;

typedef struct {
    const char *sending;   /* "enabled", "disabled", or NULL to omit */
    const char *receiving; /* "enabled", "disabled", or NULL to omit */
} resend_capabilities; /* NULL omits "capabilities"; {0} sends {} and the API decides. */

typedef struct {
    char *id;
    char *name;
    char *status;
    char *region;
    char *sending;   /* capabilities.sending */
    char *receiving; /* capabilities.receiving */
    int open_tracking;  /* 0 when missing, null, or false */
    int click_tracking; /* 0 when missing, null, or false */
    char *tracking_subdomain;       /* NULL on list */
    resend_dns_record *records;     /* NULL on list; set on get and create */
    size_t record_count;
    char *created_at;
    /* tls and custom_return_path are inputs only; the API never returns them. */
} resend_domain;

typedef struct {
    resend_domain *items;
    size_t count;
    int has_more;
} resend_domain_list;

typedef struct {
    const char *name; /* required */
    const char *region; /* "us-east-1" (default), "eu-west-1", "sa-east-1", "ap-northeast-1" */
    const char *custom_return_path;
    const char *tls; /* "opportunistic" or "enforced", or NULL */
    const char *tracking_subdomain;
    const int *open_tracking;
    const int *click_tracking;
    const resend_capabilities *capabilities;
} resend_domain_create;

/* At least one field is set. NULL leaves that field unchanged. */
typedef struct {
    const char *tls;
    const char *tracking_subdomain;
    const int *open_tracking;
    const int *click_tracking;
    const resend_capabilities *capabilities;
} resend_domain_update;

int resend_domains_list(resend *client, const resend_page *page, resend_domain_list *out);
int resend_domains_get(resend *client, const char *domain_id, resend_domain *out);
int resend_domains_create(resend *client, const resend_domain_create *domain, resend_domain *out);
int resend_domains_verify(resend *client, const char *domain_id);
int resend_domains_update(resend *client, const char *domain_id, const resend_domain_update *domain);
int resend_domains_remove(resend *client, const char *domain_id);

void resend_domain_free(resend_domain *domain);
void resend_domain_list_free(resend_domain_list *list);

/* ---- API keys ---------------------------------------------------------- */

typedef struct {
    const char *name;       /* required */
    const char *permission; /* "full_access" or "sending_access", or NULL */
    const char *domain_id;  /* optional; only with sending_access */
} resend_api_key_create;

typedef struct {
    char *id;
    char *name;         /* NULL on create */
    char *created_at;   /* NULL on create */
    char *last_used_at; /* list; NULL if the API omitted it */
    char *token;        /* set only on create */
} resend_api_key;

typedef struct {
    resend_api_key *items;
    size_t count;
    int has_more;
} resend_api_key_list;

int resend_api_keys_create(resend *client, const resend_api_key_create *key, resend_api_key *out);
int resend_api_keys_list(resend *client, const resend_page *page, resend_api_key_list *out);
/* Renames the key. name is required. */
int resend_api_keys_update(resend *client, const char *api_key_id, const char *name);
int resend_api_keys_remove(resend *client, const char *api_key_id);

void resend_api_key_free(resend_api_key *key);
void resend_api_key_list_free(resend_api_key_list *list);

#ifdef __cplusplus
}
#endif

#endif /* RESEND_H */
