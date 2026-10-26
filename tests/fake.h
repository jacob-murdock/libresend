#ifndef FAKE_H
#define FAKE_H

#include <stddef.h>

/*
 * Scripted exchanges on 127.0.0.1, in a thread of the check process.
 * Each connection consumes the next exchange (the last one repeats).
 * status, body, and optional Location are what the client reads back.
 * "127.0.0.1:0" in a Location is replaced with this listener's port.
 * The first request is kept: method, path, headers, body, and a count.
 * A run where the client never connects leaves the count at 0.
 *
 * fake_start copies bodies and locations. The script need not outlive it.
 * Pointers from the accessors stay valid until fake_stop.
 */

typedef struct fake fake;

typedef struct {
    int status;
    const char *body;     /* NULL means empty */
    const char *location; /* NULL omits Location */
} fake_exchange;

fake *fake_start(const fake_exchange *exchanges, size_t count);
int fake_port(fake *f);
void fake_stop(fake *f);

int fake_request_count(fake *f);
const char *fake_method(fake *f);
const char *fake_path(fake *f);
const char *fake_header(fake *f, const char *name);
const char *fake_body(fake *f);

#endif
