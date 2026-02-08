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

    /* Extract host:port */
    size_t hp_len = (size_t)(slash - hp);
    const char *colon = memchr(hp, ':', hp_len);
    if (colon) {
        size_t h_len = (size_t)(colon - hp);
        if (h_len >= host_sz) return -1;
        memcpy(host, hp, h_len);
        host[h_len] = '\0';

        size_t p_len = (size_t)(slash - colon - 1);
        if (p_len >= port_sz || p_len == 0) return -1;
        memcpy(port, colon + 1, p_len);
        port[p_len] = '\0';
    } else {
        if (hp_len >= host_sz) return -1;
        memcpy(host, hp, hp_len);
        host[hp_len] = '\0';
        snprintf(port, port_sz, "80");
    }

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

    /* Connect */
    int fd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (fd < 0) {
        freeaddrinfo(res);
        return -1;
    }

    if (connect(fd, res->ai_addr, res->ai_addrlen) < 0) {
        close(fd);
        freeaddrinfo(res);
        return -1;
    }
    freeaddrinfo(res);

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
    ssize_t sent = 0;
    while ((size_t)sent < (size_t)hlen) {
        ssize_t n = write(fd, header + sent, (size_t)hlen - (size_t)sent);
        if (n <= 0) {
            close(fd);
            return -1;
        }
        sent += n;
    }

    /* Send body */
    sent = 0;
    while ((size_t)sent < len) {
        ssize_t n = write(fd, buf + sent, len - (size_t)sent);
        if (n <= 0) {
            close(fd);
            return -1;
        }
        sent += n;
    }

    /* Read response status line */
    char resp[256];
    ssize_t nread = read(fd, resp, sizeof(resp) - 1);
    close(fd);

    if (nread <= 0) return -1;
    resp[nread] = '\0';

    /* Parse "HTTP/1.x NNN ..." */
    int status = 0;
    if (sscanf(resp, "HTTP/%*d.%*d %d", &status) != 1) return -1;

    return status;
}
