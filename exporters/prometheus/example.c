/**
 * @file example.c
 * @brief Example: AuraIO Prometheus metrics endpoint
 *
 * Demonstrates how to serve AuraIO metrics via a simple HTTP server.
 * In production, integrate auraio_metrics_prometheus() into your
 * existing HTTP server framework.
 *
 * Build:
 *   make -C /path/to/AuraIO exporters
 *
 * Usage:
 *   ./prometheus_example
 *   curl http://localhost:9091/metrics
 */

#include <auraio.h>
#include "auraio_prometheus.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

/* Best-effort write for HTTP responses — failure is not actionable */
static inline void send_bytes(int fd, const void *buf, size_t len) {
    ssize_t n = write(fd, buf, len);
    (void)n;
}

#define METRICS_PORT 9091
#define METRICS_BUF_SIZE (256 * 1024)
#define REQUEST_BUF_SIZE 4096

static volatile sig_atomic_t running = 1;

static void handle_signal(int sig) {
    (void)sig;
    running = 0;
}

static void serve_metrics(int client_fd, auraio_engine_t *engine) {
    char *buf = malloc(METRICS_BUF_SIZE);
    if (!buf) {
        const char *err = "HTTP/1.1 500 Internal Server Error\r\n\r\n";
        send_bytes(client_fd, err, strlen(err));
        return;
    }

    int len = auraio_metrics_prometheus(engine, buf, METRICS_BUF_SIZE);
    if (len < 0) {
        const char *err = (errno == ENOBUFS) ? "HTTP/1.1 500 Buffer Too Small\r\n\r\n"
                                              : "HTTP/1.1 500 Internal Server Error\r\n\r\n";
        send_bytes(client_fd, err, strlen(err));
        free(buf);
        return;
    }

    char header[256];
    int hlen = snprintf(header, sizeof(header),
                        "HTTP/1.1 200 OK\r\n"
                        "Content-Type: text/plain; version=0.0.4; charset=utf-8\r\n"
                        "Content-Length: %d\r\n"
                        "\r\n", len);
    if (hlen >= (int)sizeof(header)) hlen = (int)sizeof(header) - 1;

    send_bytes(client_fd, header, hlen);
    send_bytes(client_fd, buf, len);
    free(buf);
}

static void serve_404(int client_fd) {
    const char *resp =
        "HTTP/1.1 404 Not Found\r\n"
        "Content-Length: 39\r\n"
        "\r\n"
        "Not Found. Try GET /metrics instead.\r\n";
    send_bytes(client_fd, resp, strlen(resp));
}

int main(void) {
    signal(SIGINT, handle_signal);
    signal(SIGTERM, handle_signal);
    signal(SIGPIPE, SIG_IGN);

    /* Create a minimal AuraIO engine for demonstration */
    auraio_options_t opts;
    auraio_options_init(&opts);
    opts.ring_count = 1;
    opts.queue_depth = 64;

    auraio_engine_t *engine = auraio_create_with_options(&opts);
    if (!engine) {
        fprintf(stderr, "Failed to create AuraIO engine: %s\n", strerror(errno));
        return 1;
    }

    printf("AuraIO Prometheus Exporter (v%s)\n", auraio_version());
    printf("Engine: %d ring(s), queue_depth=%d\n",
           auraio_get_ring_count(engine), opts.queue_depth);

    /* Set up TCP server */
    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) {
        perror("socket");
        auraio_destroy(engine);
        return 1;
    }

    int optval = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval));

    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port = htons(METRICS_PORT),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };

    if (bind(server_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind");
        close(server_fd);
        auraio_destroy(engine);
        return 1;
    }

    if (listen(server_fd, 8) < 0) {
        perror("listen");
        close(server_fd);
        auraio_destroy(engine);
        return 1;
    }

    printf("Listening on http://localhost:%d/metrics\n", METRICS_PORT);
    printf("Press Ctrl+C to stop.\n\n");

    while (running) {
        int client_fd = accept(server_fd, NULL, NULL);
        if (client_fd < 0) {
            if (errno == EINTR) continue;
            perror("accept");
            break;
        }

        /* Set read timeout — this is a demo server; a slow/stuck client
         * should not block the accept loop indefinitely. */
        struct timeval tv = { .tv_sec = 5, .tv_usec = 0 };
        setsockopt(client_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

        /* Read request (minimal HTTP parsing) */
        char req[REQUEST_BUF_SIZE];
        ssize_t nread = read(client_fd, req, sizeof(req) - 1);
        if (nread > 0) {
            req[nread] = '\0';

            if (strncmp(req, "GET /metrics", 12) == 0) {
                serve_metrics(client_fd, engine);
            } else {
                serve_404(client_fd);
            }
        }

        close(client_fd);
    }

    printf("\nShutting down...\n");
    close(server_fd);
    auraio_destroy(engine);
    return 0;
}
