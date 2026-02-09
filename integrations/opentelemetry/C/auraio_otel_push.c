/**
 * @file auraio_otel_push.c
 * @brief HTTP push helper for OTLP JSON payloads
 *
 * Simple blocking HTTP POST using POSIX sockets.
 * No TLS, no keepalive, no chunked encoding, no external dependencies.
 */

#define _POSIX_C_SOURCE 200809L

#include "auraio_otel_push.h"

#include <errno.h>
#include <netdb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

/* Parse "http://host:port/path" into components.
 * Returns 0 on success, -1 on parse error. */
static int parse_endpoint(const char *endpoint, char *host, size_t host_sz, char *port,
                          size_t port_sz, const char **path_out) {
    /* Skip "http://" */
    if (strncmp(endpoint, "http://", 7) != 0) return -1;
    const char *hp = endpoint + 7;

    /* Find path start */
    const char *slash = strchr(hp, '/');
    if (!slash) {
        *path_out = "/";
        slash = hp + strlen(hp);
    } else {
        *path_out = slash;
    }

    /* Extract host[:port] authority portion */
    size_t hp_len = (size_t)(slash - hp);
    if (hp_len == 0) return -1;

    if (hp[0] == '[') {
        /* Bracketed IPv6 literal: [addr]:port */
        const char *rbr = memchr(hp, ']', hp_len);
        if (!rbr) return -1;

        size_t h_len = (size_t)(rbr - (hp + 1));
        if (h_len == 0 || h_len >= host_sz) return -1;
        memcpy(host, hp + 1, h_len);
        host[h_len] = '\0';

        if ((size_t)(rbr - hp + 1) == hp_len) {
            snprintf(port, port_sz, "80");
            return 0;
        }

        if (*(rbr + 1) != ':') return -1;
        size_t p_len = (size_t)(hp + hp_len - (rbr + 2));
        if (p_len == 0 || p_len >= port_sz) return -1;
        memcpy(port, rbr + 2, p_len);
        port[p_len] = '\0';
    } else {
        /* Hostname / IPv4 with optional :port */
        const char *colon = memchr(hp, ':', hp_len);
        if (colon) {
            /* Reject unbracketed multiple-colon authorities (ambiguous IPv6). */
            if (memchr(colon + 1, ':', (size_t)(hp + hp_len - (colon + 1))) != NULL) return -1;

            size_t h_len = (size_t)(colon - hp);
            if (h_len == 0 || h_len >= host_sz) return -1;
            memcpy(host, hp, h_len);
            host[h_len] = '\0';

            size_t p_len = (size_t)(hp + hp_len - (colon + 1));
            if (p_len == 0 || p_len >= port_sz) return -1;
            memcpy(port, colon + 1, p_len);
            port[p_len] = '\0';
        } else {
            if (hp_len == 0 || hp_len >= host_sz) return -1;
            memcpy(host, hp, hp_len);
            host[hp_len] = '\0';
            snprintf(port, port_sz, "80");
        }
    }

    return 0;
}

static int send_all(int fd, const char *buf, size_t len) {
    size_t sent = 0;
    while (sent < len) {
        ssize_t n = send(fd, buf + sent, len - sent,
#ifdef MSG_NOSIGNAL
                         MSG_NOSIGNAL
#else
                         0
#endif
        );
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (n == 0) return -1;
        sent += (size_t)n;
    }
    return 0;
}

static int read_status_line(int fd, char *resp, size_t resp_sz) {
    size_t used = 0;
    while (used + 1 < resp_sz) {
        ssize_t n = read(fd, resp + used, 1);
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (n == 0) break;
        if (resp[used] == '\n') {
            used++;
            break;
        }
        used++;
    }
    if (used == 0) return -1;
    resp[used] = '\0';
    return 0;
}

int auraio_otel_push(const char *endpoint, const char *buf, size_t len) {
    if (!endpoint || !buf || len == 0) return -1;

    char host[256];
    char port[16];
    const char *path;

    if (parse_endpoint(endpoint, host, sizeof(host), port, sizeof(port), &path) < 0) {
        return -1;
    }

    /* DNS resolve */
    struct addrinfo hints = { .ai_family = AF_UNSPEC, .ai_socktype = SOCK_STREAM };
    struct addrinfo *res = NULL;
    if (getaddrinfo(host, port, &hints, &res) != 0 || !res) {
        return -1;
    }

    /* Connect: try all returned addresses */
    int fd = -1;
    for (struct addrinfo *ai = res; ai; ai = ai->ai_next) {
        int candidate = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (candidate < 0) continue;

        int rc;
        do {
            rc = connect(candidate, ai->ai_addr, ai->ai_addrlen);
        } while (rc < 0 && errno == EINTR);

        if (rc == 0) {
            fd = candidate;
            break;
        }
        close(candidate);
    }
    freeaddrinfo(res);
    if (fd < 0) return -1;

    /* Build and send HTTP request header */
    char header[1024];
    int hlen = snprintf(header, sizeof(header),
                        "POST %s HTTP/1.1\r\n"
                        "Host: %s:%s\r\n"
                        "Content-Type: application/json\r\n"
                        "Content-Length: %zu\r\n"
                        "Connection: close\r\n"
                        "\r\n",
                        path, host, port, len);
    if (hlen < 0 || (size_t)hlen >= sizeof(header)) {
        close(fd);
        return -1;
    }

    /* Send header */
    if (send_all(fd, header, (size_t)hlen) < 0) {
        close(fd);
        return -1;
    }

    /* Send body */
    if (send_all(fd, buf, len) < 0) {
        close(fd);
        return -1;
    }

    /* Read response status line */
    char resp[256];
    int read_ok = read_status_line(fd, resp, sizeof(resp));
    close(fd);

    if (read_ok < 0) return -1;

    /* Parse "HTTP/1.x NNN ..." */
    int status = 0;
    if (sscanf(resp, "HTTP/%*d.%*d %d", &status) != 1) return -1;

    return status;
}
