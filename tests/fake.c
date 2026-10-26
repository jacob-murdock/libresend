#include "fake.h"

#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

enum { FAKE_MAX = 1024 * 1024, FAKE_HEADERS = 64 };

struct header {
    char *name;
    char *value;
};

struct canned {
    int status;
    char *body;
    size_t body_len;
    char *location;
};

struct fake {
    int listen_fd;
    int wake_r;
    int wake_w;
    int port;
    struct canned *script;
    size_t script_len;
    size_t script_i;
    pthread_t thread;
    pthread_mutex_t mu;
    int request_count;
    char *method;
    char *path;
    struct header headers[FAKE_HEADERS];
    int header_count;
    char *req_body;
};

struct buf {
    char *data;
    size_t len;
    size_t cap;
};

static int name_eq(const char *a, const char *b) {
    while (*a && *b) {
        unsigned char ca = (unsigned char)*a++;
        unsigned char cb = (unsigned char)*b++;
        if (tolower(ca) != tolower(cb))
            return 0;
    }
    return *a == *b;
}

static int contains_ci(const char *s, const char *needle) {
    size_t n = strlen(needle);
    if (n == 0)
        return 1;
    for (; *s; s++) {
        size_t i = 0;
        while (i < n && s[i] && tolower((unsigned char)s[i]) == tolower((unsigned char)needle[i]))
            i++;
        if (i == n)
            return 1;
    }
    return 0;
}

static const char *header_get(const struct header *hs, int n, const char *name) {
    for (int i = 0; i < n; i++) {
        if (name_eq(hs[i].name, name))
            return hs[i].value;
    }
    return NULL;
}

static const char *reason_for(int status) {
    switch (status) {
    case 100:
        return "Continue";
    case 200:
        return "OK";
    case 302:
        return "Found";
    case 400:
        return "Bad Request";
    case 401:
        return "Unauthorized";
    case 404:
        return "Not Found";
    case 422:
        return "Unprocessable Entity";
    case 500:
        return "Internal Server Error";
    default:
        return "Error";
    }
}

static int write_all(int fd, const void *buf, size_t len) {
    const char *p = buf;
    while (len) {
        ssize_t n = send(fd, p, len, MSG_NOSIGNAL);
        if (n < 0) {
            if (errno == EINTR || errno == EAGAIN)
                continue;
            return -1;
        }
        if (n == 0)
            return -1;
        p += n;
        len -= (size_t)n;
    }
    return 0;
}

static int read_some(fake *f, int fd, void *dst, size_t cap) {
    for (;;) {
        struct pollfd pfd[2];
        pfd[0].fd = fd;
        pfd[0].events = POLLIN;
        pfd[1].fd = f->wake_r;
        pfd[1].events = POLLIN;
        int rc = poll(pfd, 2, -1);
        if (rc < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        if (pfd[1].revents)
            return -1;
        if (pfd[0].revents & (POLLIN | POLLHUP | POLLERR)) {
            ssize_t n = recv(fd, dst, cap, 0);
            if (n < 0) {
                if (errno == EINTR)
                    continue;
                return -1;
            }
            return (int)n;
        }
    }
}

static int buf_append(struct buf *b, const char *p, size_t n) {
    if (n > FAKE_MAX || b->len > FAKE_MAX - n)
        return -1;
    size_t need = b->len + n + 1;
    if (need > b->cap) {
        size_t cap = b->cap ? b->cap : 4096;
        while (cap < need) {
            if (cap > (size_t)FAKE_MAX / 2) {
                cap = (size_t)FAKE_MAX + 1;
                break;
            }
            cap *= 2;
        }
        if (cap < need)
            return -1;
        char *grown = realloc(b->data, cap);
        if (!grown)
            return -1;
        b->data = grown;
        b->cap = cap;
    }
    memcpy(b->data + b->len, p, n);
    b->len += n;
    b->data[b->len] = '\0';
    return 0;
}

static char *find_eoh(char *data, size_t len) {
    if (!data || len < 4)
        return NULL;
    for (size_t i = 0; i + 3 < len; i++) {
        if (data[i] == '\r' && data[i + 1] == '\n' && data[i + 2] == '\r' && data[i + 3] == '\n')
            return data + i;
    }
    return NULL;
}

static int read_headers(fake *f, int fd, struct buf *b) {
    for (;;) {
        if (find_eoh(b->data, b->len))
            return 0;
        if (b->len >= FAKE_MAX)
            return -1;
        char tmp[2048];
        size_t room = FAKE_MAX - b->len;
        if (room > sizeof tmp)
            room = sizeof tmp;
        int n = read_some(f, fd, tmp, room);
        if (n <= 0)
            return -1;
        if (buf_append(b, tmp, (size_t)n) != 0)
            return -1;
    }
}

static int parse_request_line(char *line, char **method, char **path) {
    *method = NULL;
    *path = NULL;
    char *sp = strchr(line, ' ');
    if (!sp)
        return -1;
    *sp = '\0';
    char *sp2 = strchr(sp + 1, ' ');
    if (!sp2)
        return -1;
    *sp2 = '\0';
    *method = strdup(line);
    *path = strdup(sp + 1);
    if (!*method || !*path)
        return -1;
    return 0;
}

static int parse_block(char *block, char **method, char **path, struct header *hs, int *nh) {
    *nh = 0;
    char *line = block;
    char *next = strstr(line, "\r\n");
    if (!next)
        return -1;
    *next = '\0';
    if (parse_request_line(line, method, path) != 0)
        return -1;

    line = next + 2;
    while (*line) {
        next = strstr(line, "\r\n");
        if (!next)
            return -1;
        *next = '\0';
        if (line[0] != '\0') {
            char *colon = strchr(line, ':');
            if (!colon || *nh >= FAKE_HEADERS)
                return -1;
            *colon = '\0';
            char *val = colon + 1;
            while (*val == ' ' || *val == '\t')
                val++;
            char *name = strdup(line);
            char *value = strdup(val);
            if (!name || !value) {
                free(name);
                free(value);
                return -1;
            }
            hs[*nh].name = name;
            hs[*nh].value = value;
            (*nh)++;
        }
        line = next + 2;
    }
    return 0;
}

static int content_length(const struct header *hs, int n, size_t *out) {
    const char *v = header_get(hs, n, "Content-Length");
    if (!v) {
        *out = 0;
        return 0;
    }
    errno = 0;
    char *end = NULL;
    unsigned long long x = strtoull(v, &end, 10);
    if (end == v || errno == ERANGE)
        return -1;
    while (*end == ' ' || *end == '\t')
        end++;
    if (*end != '\0' || x > FAKE_MAX)
        return -1;
    *out = (size_t)x;
    return 0;
}

/* Returns 1 when this request is the one the accessors will show. */
static int publish(fake *f, char *method, char *path, struct header *hs, int nh, char *body) {
    pthread_mutex_lock(&f->mu);
    int first = f->request_count == 0;
    f->request_count++;
    if (first) {
        f->method = method;
        f->path = path;
        f->header_count = nh;
        if (nh)
            memcpy(f->headers, hs, (size_t)nh * sizeof *hs);
        f->req_body = body;
    }
    pthread_mutex_unlock(&f->mu);
    return first;
}

static const struct canned *next_canned(fake *f) {
    pthread_mutex_lock(&f->mu);
    size_t i = f->script_i;
    if (i + 1 < f->script_len)
        f->script_i++;
    pthread_mutex_unlock(&f->mu);
    return &f->script[i];
}

static int write_response(int fd, const struct canned *c) {
    char hdr[1024];
    int n;
    if (c->location) {
        n = snprintf(hdr, sizeof hdr,
                     "HTTP/1.1 %d %s\r\n"
                     "Location: %s\r\n"
                     "Content-Type: application/json\r\n"
                     "Content-Length: %zu\r\n"
                     "Connection: close\r\n"
                     "\r\n",
                     c->status, reason_for(c->status), c->location, c->body_len);
    } else {
        n = snprintf(hdr, sizeof hdr,
                     "HTTP/1.1 %d %s\r\n"
                     "Content-Type: application/json\r\n"
                     "Content-Length: %zu\r\n"
                     "Connection: close\r\n"
                     "\r\n",
                     c->status, reason_for(c->status), c->body_len);
    }
    if (n < 0 || (size_t)n >= sizeof hdr)
        return -1;
    if (write_all(fd, hdr, (size_t)n) != 0)
        return -1;
    if (c->body_len != 0 && write_all(fd, c->body, c->body_len) != 0)
        return -1;
    return 0;
}

static void free_headers(struct header *hs, int n) {
    for (int i = 0; i < n; i++) {
        free(hs[i].name);
        free(hs[i].value);
    }
}

static void handle_client(fake *f, int fd) {
    struct buf b = {0};
    struct header hs[FAKE_HEADERS] = {0};
    int nh = 0;
    char *method = NULL;
    char *path = NULL;
    char *body = NULL;
    char *marker = NULL;
    const char *already = NULL;
    const char *expect = NULL;
    size_t have = 0;
    size_t need = 0;
    size_t got = 0;
    int stored = 0;

    if (read_headers(f, fd, &b) != 0)
        goto done;
    marker = find_eoh(b.data, b.len);
    if (!marker)
        goto done;
    /* marker is the CRLF that ends the last header line. The blank line
     * is the next two bytes. Keep that CRLF and end the block after it. */
    have = b.len - (size_t)((marker + 4) - b.data);
    already = marker + 4;
    marker[2] = '\0';

    if (parse_block(b.data, &method, &path, hs, &nh) != 0)
        goto done;
    if (content_length(hs, nh, &need) != 0)
        goto done;
    if (have > need)
        have = need;

    expect = header_get(hs, nh, "Expect");
    if (expect && contains_ci(expect, "100-continue") && have < need) {
        static const char cont[] = "HTTP/1.1 100 Continue\r\n\r\n";
        if (write_all(fd, cont, sizeof cont - 1) != 0)
            goto done;
    }

    body = malloc(need + 1);
    if (!body)
        goto done;
    if (have)
        memcpy(body, already, have);
    got = have;
    while (got < need) {
        int n = read_some(f, fd, body + got, need - got);
        if (n <= 0)
            goto done;
        got += (size_t)n;
    }
    body[need] = '\0';

    if (publish(f, method, path, hs, nh, body))
        stored = 1;
    write_response(fd, next_canned(f));

done:
    if (!stored) {
        free(method);
        free(path);
        free(body);
        free_headers(hs, nh);
    }
    free(b.data);
}

static void *serve(void *arg) {
    fake *f = arg;
    for (;;) {
        struct pollfd pfd[2];
        pfd[0].fd = f->listen_fd;
        pfd[0].events = POLLIN;
        pfd[1].fd = f->wake_r;
        pfd[1].events = POLLIN;
        int rc = poll(pfd, 2, -1);
        if (rc < 0) {
            if (errno == EINTR)
                continue;
            break;
        }
        if (pfd[1].revents)
            break;
        if (pfd[0].revents & (POLLERR | POLLNVAL | POLLHUP))
            break;
        if (!(pfd[0].revents & POLLIN))
            continue;
        int fd = accept(f->listen_fd, NULL, NULL);
        if (fd < 0) {
            if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)
                continue;
            break;
        }
        handle_client(f, fd);
        close(fd);
    }
    return NULL;
}

static void free_script(struct canned *script, size_t n) {
    if (!script)
        return;
    for (size_t i = 0; i < n; i++) {
        free(script[i].body);
        free(script[i].location);
    }
    free(script);
}

static void fake_fail(fake *f) {
    if (!f)
        return;
    if (f->wake_w >= 0)
        close(f->wake_w);
    if (f->wake_r >= 0)
        close(f->wake_r);
    if (f->listen_fd >= 0)
        close(f->listen_fd);
    free_script(f->script, f->script_len);
    free(f);
}

/* "127.0.0.1:0" stands for this listener. The port is assigned at bind. */
static int rewrite_listener_port(char **location, int port) {
    if (!*location)
        return 0;
    const char *mark = strstr(*location, "127.0.0.1:0");
    if (!mark)
        return 0;
    const char *rest = mark + strlen("127.0.0.1:0");
    if (*rest >= '0' && *rest <= '9')
        return 0;
    char buf[512];
    int n = snprintf(buf, sizeof buf, "%.*s127.0.0.1:%d%s", (int)(mark - *location), *location, port,
                     rest);
    if (n < 0 || (size_t)n >= sizeof buf)
        return -1;
    char *copy = strdup(buf);
    if (!copy)
        return -1;
    free(*location);
    *location = copy;
    return 0;
}

fake *fake_start(const fake_exchange *exchanges, size_t count) {
    if (!exchanges || count == 0)
        return NULL;

    fake *f = calloc(1, sizeof *f);
    if (!f)
        return NULL;
    f->listen_fd = -1;
    f->wake_r = -1;
    f->wake_w = -1;

    f->script = calloc(count, sizeof *f->script);
    if (!f->script) {
        fake_fail(f);
        return NULL;
    }
    f->script_len = count;
    for (size_t i = 0; i < count; i++) {
        const char *body = exchanges[i].body ? exchanges[i].body : "";
        f->script[i].status = exchanges[i].status;
        f->script[i].body_len = strlen(body);
        f->script[i].body = malloc(f->script[i].body_len + 1);
        if (!f->script[i].body) {
            fake_fail(f);
            return NULL;
        }
        memcpy(f->script[i].body, body, f->script[i].body_len + 1);
        if (exchanges[i].location) {
            f->script[i].location = strdup(exchanges[i].location);
            if (!f->script[i].location) {
                fake_fail(f);
                return NULL;
            }
        }
    }

    f->listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (f->listen_fd < 0) {
        fake_fail(f);
        return NULL;
    }
    int flags = fcntl(f->listen_fd, F_GETFL, 0);
    if (flags < 0 || fcntl(f->listen_fd, F_SETFL, flags | O_NONBLOCK) != 0) {
        fake_fail(f);
        return NULL;
    }

    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(0);
    if (bind(f->listen_fd, (struct sockaddr *)&addr, sizeof addr) != 0 ||
        listen(f->listen_fd, 8) != 0) {
        fake_fail(f);
        return NULL;
    }
    socklen_t alen = sizeof addr;
    if (getsockname(f->listen_fd, (struct sockaddr *)&addr, &alen) != 0) {
        fake_fail(f);
        return NULL;
    }
    f->port = ntohs(addr.sin_port);
    if (f->port == 0) {
        fake_fail(f);
        return NULL;
    }
    for (size_t i = 0; i < f->script_len; i++) {
        if (rewrite_listener_port(&f->script[i].location, f->port) != 0) {
            fake_fail(f);
            return NULL;
        }
    }

    int wake[2];
    if (pipe(wake) != 0) {
        fake_fail(f);
        return NULL;
    }
    f->wake_r = wake[0];
    f->wake_w = wake[1];

    if (pthread_mutex_init(&f->mu, NULL) != 0) {
        fake_fail(f);
        return NULL;
    }
    if (pthread_create(&f->thread, NULL, serve, f) != 0) {
        pthread_mutex_destroy(&f->mu);
        fake_fail(f);
        return NULL;
    }
    return f;
}

int fake_port(fake *f) { return f ? f->port : 0; }

static void lock(fake *f) { pthread_mutex_lock(&f->mu); }
static void unlock(fake *f) { pthread_mutex_unlock(&f->mu); }

int fake_request_count(fake *f) {
    if (!f)
        return 0;
    lock(f);
    int n = f->request_count;
    unlock(f);
    return n;
}

const char *fake_method(fake *f) {
    if (!f)
        return NULL;
    lock(f);
    const char *s = f->method;
    unlock(f);
    return s;
}

const char *fake_path(fake *f) {
    if (!f)
        return NULL;
    lock(f);
    const char *s = f->path;
    unlock(f);
    return s;
}

const char *fake_header(fake *f, const char *name) {
    if (!f || !name)
        return NULL;
    lock(f);
    const char *val = NULL;
    for (int i = 0; i < f->header_count; i++) {
        if (name_eq(f->headers[i].name, name)) {
            val = f->headers[i].value;
            break;
        }
    }
    unlock(f);
    return val;
}

const char *fake_body(fake *f) {
    if (!f)
        return NULL;
    lock(f);
    const char *s = f->req_body;
    unlock(f);
    return s;
}

void fake_stop(fake *f) {
    if (!f)
        return;
    /* Closing the write end wakes poll in the server thread. */
    if (f->wake_w >= 0) {
        close(f->wake_w);
        f->wake_w = -1;
    }
    pthread_join(f->thread, NULL);
    pthread_mutex_destroy(&f->mu);
    if (f->listen_fd >= 0)
        close(f->listen_fd);
    if (f->wake_r >= 0)
        close(f->wake_r);
    free_script(f->script, f->script_len);
    free(f->method);
    free(f->path);
    free(f->req_body);
    free_headers(f->headers, f->header_count);
    free(f);
}
