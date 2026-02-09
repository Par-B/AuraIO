/**
 * @file test_otel.c
 * @brief Unit tests for OpenTelemetry OTLP/JSON metrics exporter
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <stdatomic.h>
#include <pthread.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>

#include "../engine/include/auraio.h"
#include "../integrations/opentelemetry/C/auraio_otel.h"
#include "../integrations/opentelemetry/C/auraio_otel_push.h"

static int test_count = 0;

#define TEST(name) static void test_##name(void)
#define RUN_TEST(name)            \
    do {                          \
        printf("  %-45s", #name); \
        test_##name();            \
        printf(" OK\n");          \
        test_count++;             \
    } while (0)

/* ============================================================================
 * Helpers
 * ============================================================================ */

static char test_file[256];
static int test_fd = -1;

static void cleanup_atexit(void) {
    if (test_fd >= 0) {
        close(test_fd);
        test_fd = -1;
    }
    if (test_file[0]) {
        unlink(test_file);
    }
}

static void io_setup(void) {
    strcpy(test_file, "/tmp/test_otel_XXXXXX");
    test_fd = mkstemp(test_file);
    if (test_fd < 0) {
        strcpy(test_file, "./test_otel_XXXXXX");
        test_fd = mkstemp(test_file);
    }
    assert(test_fd >= 0);

    char data[4096];
    memset(data, 'A', sizeof(data));
    ssize_t w = write(test_fd, data, sizeof(data));
    assert(w == sizeof(data));
    lseek(test_fd, 0, SEEK_SET);
}

static void io_teardown(void) {
    if (test_fd >= 0) {
        close(test_fd);
        test_fd = -1;
    }
    unlink(test_file);
}

static _Atomic int callback_called;

static void test_callback(auraio_request_t *req, ssize_t result, void *user_data) {
    (void)req;
    (void)user_data;
    (void)result;
    callback_called = 1;
}

/* Helper: check that str contains substr */
static int contains(const char *str, const char *substr) {
    return strstr(str, substr) != NULL;
}

/* Helper: count occurrences of ch in str */
static int count_char(const char *str, char ch) {
    int n = 0;
    for (; *str; str++) {
        if (*str == ch) n++;
    }
    return n;
}

static auraio_engine_t *make_engine(int rings, int qdepth) {
    auraio_options_t opts;
    auraio_options_init(&opts);
    opts.ring_count = rings;
    opts.queue_depth = qdepth;
    return auraio_create_with_options(&opts);
}

struct push_server_ctx {
    int listen_fd;
    int port;
    int status_code;
    char request[8192];
    size_t request_len;
    pthread_t thread;
};

static void *push_server_thread(void *arg) {
    struct push_server_ctx *ctx = arg;

    int client_fd;
    for (;;) {
        client_fd = accept(ctx->listen_fd, NULL, NULL);
        if (client_fd >= 0) break;
        if (errno == EINTR) continue;
        close(ctx->listen_fd);
        ctx->listen_fd = -1;
        return NULL;
    }

    size_t used = 0;
    size_t expected_total = 0;
    int have_expected = 0;

    while (used + 1 < sizeof(ctx->request)) {
        ssize_t n = read(client_fd, ctx->request + used, sizeof(ctx->request) - 1 - used);
        if (n < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (n == 0) break;
        used += (size_t)n;
        ctx->request[used] = '\0';

        if (!have_expected) {
            char *header_end = strstr(ctx->request, "\r\n\r\n");
            if (header_end) {
                size_t header_len = (size_t)(header_end - ctx->request) + 4;
                size_t body_len = 0;
                char *cl = strstr(ctx->request, "Content-Length:");
                if (cl) body_len = (size_t)strtoul(cl + strlen("Content-Length:"), NULL, 10);
                expected_total = header_len + body_len;
                have_expected = 1;
            }
        }

        if (have_expected && used >= expected_total) break;
    }

    ctx->request_len = used;
    ctx->request[used] = '\0';

    char resp[128];
    int nresp = snprintf(resp, sizeof(resp), "HTTP/1.1 %d Test\r\nContent-Length: 0\r\n\r\n",
                         ctx->status_code);
    if (nresp > 0) {
        size_t sent = 0;
        while (sent < (size_t)nresp) {
            ssize_t n = write(client_fd, resp + sent, (size_t)nresp - sent);
            if (n < 0) {
                if (errno == EINTR) continue;
                break;
            }
            if (n == 0) break;
            sent += (size_t)n;
        }
    }

    close(client_fd);
    close(ctx->listen_fd);
    ctx->listen_fd = -1;
    return NULL;
}

static int push_server_start(struct push_server_ctx *ctx, int status_code) {
    memset(ctx, 0, sizeof(*ctx));
    ctx->listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (ctx->listen_fd < 0) return -1;

    int one = 1;
    setsockopt(ctx->listen_fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = 0;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    if (bind(ctx->listen_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(ctx->listen_fd);
        ctx->listen_fd = -1;
        return -1;
    }
    if (listen(ctx->listen_fd, 1) < 0) {
        close(ctx->listen_fd);
        ctx->listen_fd = -1;
        return -1;
    }

    socklen_t alen = sizeof(addr);
    if (getsockname(ctx->listen_fd, (struct sockaddr *)&addr, &alen) < 0) {
        close(ctx->listen_fd);
        ctx->listen_fd = -1;
        return -1;
    }

    ctx->port = ntohs(addr.sin_port);
    ctx->status_code = status_code;
    if (pthread_create(&ctx->thread, NULL, push_server_thread, ctx) != 0) {
        close(ctx->listen_fd);
        ctx->listen_fd = -1;
        return -1;
    }

    return 0;
}

static void push_server_join(struct push_server_ctx *ctx) {
    pthread_join(ctx->thread, NULL);
}

/* ============================================================================
 * NULL / Invalid Argument Tests
 * ============================================================================ */

TEST(null_engine) {
    char buf[64];
    int rc = auraio_metrics_otel(NULL, buf, sizeof(buf));
    assert(rc == -1);
}

TEST(null_buffer) {
    auraio_engine_t *engine = make_engine(1, 32);
    assert(engine);
    int rc = auraio_metrics_otel(engine, NULL, 1024);
    assert(rc == -1);
    auraio_destroy(engine);
}

TEST(zero_size_buffer) {
    auraio_engine_t *engine = make_engine(1, 32);
    assert(engine);
    char buf[1];
    int rc = auraio_metrics_otel(engine, buf, 0);
    assert(rc == -1);
    auraio_destroy(engine);
}

/* ============================================================================
 * Buffer Overflow Tests
 * ============================================================================ */

TEST(tiny_buffer_returns_negative) {
    auraio_engine_t *engine = make_engine(1, 32);
    assert(engine);
    char buf[64];
    int rc = auraio_metrics_otel(engine, buf, sizeof(buf));
    assert(rc < 0);
    assert(errno == ENOBUFS);
    /* abs(rc) should be a usable retry size */
    assert(-rc > (int)sizeof(buf));
    auraio_destroy(engine);
}

TEST(retry_with_estimated_size) {
    auraio_engine_t *engine = make_engine(1, 32);
    assert(engine);

    /* First call with tiny buffer */
    char small[64];
    int rc = auraio_metrics_otel(engine, small, sizeof(small));
    assert(rc < 0);
    size_t estimate = (size_t)(-rc);

    /* Retry with estimated size */
    char *big = malloc(estimate);
    assert(big);
    rc = auraio_metrics_otel(engine, big, estimate);
    assert(rc > 0);

    free(big);
    auraio_destroy(engine);
}

/* ============================================================================
 * JSON Structure Tests
 * ============================================================================ */

TEST(valid_json_braces) {
    auraio_engine_t *engine = make_engine(1, 32);
    assert(engine);

    char buf[131072];
    int len = auraio_metrics_otel(engine, buf, sizeof(buf));
    assert(len > 0);
    buf[len] = '\0';

    /* Balanced braces and brackets */
    assert(count_char(buf, '{') == count_char(buf, '}'));
    assert(count_char(buf, '[') == count_char(buf, ']'));

    /* Starts and ends correctly */
    assert(buf[0] == '{');
    assert(buf[len - 1] == '}');

    auraio_destroy(engine);
}

TEST(contains_resource_metrics) {
    auraio_engine_t *engine = make_engine(1, 32);
    assert(engine);

    char buf[131072];
    int len = auraio_metrics_otel(engine, buf, sizeof(buf));
    assert(len > 0);
    buf[len] = '\0';

    assert(contains(buf, "\"resourceMetrics\""));
    assert(contains(buf, "\"scopeMetrics\""));
    assert(contains(buf, "\"metrics\""));
    assert(contains(buf, "\"service.name\""));
    assert(contains(buf, "\"auraio\""));
    assert(contains(buf, "\"auraio.engine\""));

    auraio_destroy(engine);
}

TEST(contains_schema_version) {
    auraio_engine_t *engine = make_engine(1, 32);
    assert(engine);

    char buf[131072];
    int len = auraio_metrics_otel(engine, buf, sizeof(buf));
    assert(len > 0);
    buf[len] = '\0';

    assert(contains(buf, "\"auraio.schema.version\""));
    assert(contains(buf, AURAIO_OTEL_SCHEMA_VERSION));
    assert(contains(buf, "\"auraio.schema.stability\""));
    assert(contains(buf, AURAIO_OTEL_SCHEMA_STABILITY));

    auraio_destroy(engine);
}

TEST(contains_library_version) {
    auraio_engine_t *engine = make_engine(1, 32);
    assert(engine);

    char buf[131072];
    int len = auraio_metrics_otel(engine, buf, sizeof(buf));
    assert(len > 0);
    buf[len] = '\0';

    assert(contains(buf, auraio_version()));

    auraio_destroy(engine);
}

/* ============================================================================
 * Metric Presence Tests
 * ============================================================================ */

TEST(contains_aggregate_sums) {
    auraio_engine_t *engine = make_engine(1, 32);
    assert(engine);

    char buf[131072];
    int len = auraio_metrics_otel(engine, buf, sizeof(buf));
    assert(len > 0);
    buf[len] = '\0';

    assert(contains(buf, "\"auraio.ops.completed\""));
    assert(contains(buf, "\"auraio.bytes.transferred\""));
    assert(contains(buf, "\"auraio.adaptive.spills\""));
    /* Check sum type markers */
    assert(contains(buf, "\"isMonotonic\":true"));
    assert(contains(buf, "\"aggregationTemporality\":2"));

    auraio_destroy(engine);
}

TEST(contains_aggregate_gauges) {
    auraio_engine_t *engine = make_engine(1, 32);
    assert(engine);

    char buf[131072];
    int len = auraio_metrics_otel(engine, buf, sizeof(buf));
    assert(len > 0);
    buf[len] = '\0';

    assert(contains(buf, "\"auraio.throughput\""));
    assert(contains(buf, "\"auraio.p99_latency\""));
    assert(contains(buf, "\"auraio.in_flight\""));
    assert(contains(buf, "\"auraio.optimal_in_flight\""));
    assert(contains(buf, "\"auraio.optimal_batch_size\""));
    assert(contains(buf, "\"auraio.ring_count\""));
    assert(contains(buf, "\"gauge\""));

    auraio_destroy(engine);
}

TEST(contains_buffer_pool_metrics) {
    auraio_engine_t *engine = make_engine(1, 32);
    assert(engine);

    char buf[131072];
    int len = auraio_metrics_otel(engine, buf, sizeof(buf));
    assert(len > 0);
    buf[len] = '\0';

    assert(contains(buf, "\"auraio.buffer_pool.allocated\""));
    assert(contains(buf, "\"auraio.buffer_pool.buffers\""));
    assert(contains(buf, "\"auraio.buffer_pool.shards\""));

    auraio_destroy(engine);
}

TEST(contains_per_ring_metrics) {
    auraio_engine_t *engine = make_engine(2, 32);
    assert(engine);

    char buf[262144];
    int len = auraio_metrics_otel(engine, buf, sizeof(buf));
    assert(len > 0);
    buf[len] = '\0';

    assert(contains(buf, "\"auraio.ring.ops.completed\""));
    assert(contains(buf, "\"auraio.ring.bytes.transferred\""));
    assert(contains(buf, "\"auraio.ring.in_flight\""));
    assert(contains(buf, "\"auraio.ring.in_flight_limit\""));
    assert(contains(buf, "\"auraio.ring.batch_threshold\""));
    assert(contains(buf, "\"auraio.ring.queue_depth\""));
    assert(contains(buf, "\"auraio.ring.p99_latency\""));
    assert(contains(buf, "\"auraio.ring.throughput\""));
    assert(contains(buf, "\"auraio.ring.aimd_phase\""));

    /* Ring attribute present for both rings */
    assert(contains(buf, "\"intValue\":\"0\""));
    assert(contains(buf, "\"intValue\":\"1\""));

    auraio_destroy(engine);
}

TEST(contains_histogram) {
    auraio_engine_t *engine = make_engine(1, 32);
    assert(engine);

    char buf[131072];
    int len = auraio_metrics_otel(engine, buf, sizeof(buf));
    assert(len > 0);
    buf[len] = '\0';

    assert(contains(buf, "\"auraio.latency\""));
    assert(contains(buf, "\"histogram\""));
    assert(contains(buf, "\"explicitBounds\""));
    assert(contains(buf, "\"bucketCounts\""));
    assert(contains(buf, "\"count\""));
    assert(contains(buf, "\"sum\""));
    assert(contains(buf, "\"min\""));
    assert(contains(buf, "\"max\""));

    auraio_destroy(engine);
}

TEST(contains_timestamps) {
    auraio_engine_t *engine = make_engine(1, 32);
    assert(engine);

    char buf[131072];
    int len = auraio_metrics_otel(engine, buf, sizeof(buf));
    assert(len > 0);
    buf[len] = '\0';

    assert(contains(buf, "\"timeUnixNano\""));
    /* Cumulative sums should have startTimeUnixNano */
    assert(contains(buf, "\"startTimeUnixNano\":\"0\""));

    auraio_destroy(engine);
}

TEST(contains_units) {
    auraio_engine_t *engine = make_engine(1, 32);
    assert(engine);

    char buf[131072];
    int len = auraio_metrics_otel(engine, buf, sizeof(buf));
    assert(len > 0);
    buf[len] = '\0';

    assert(contains(buf, "\"unit\":\"{operation}\""));
    assert(contains(buf, "\"unit\":\"By\""));
    assert(contains(buf, "\"unit\":\"By/s\""));
    assert(contains(buf, "\"unit\":\"s\""));
    assert(contains(buf, "\"unit\":\"{ring}\""));
    assert(contains(buf, "\"unit\":\"{buffer}\""));
    assert(contains(buf, "\"unit\":\"{shard}\""));

    auraio_destroy(engine);
}

/* ============================================================================
 * Data Value Tests
 * ============================================================================ */

TEST(ring_count_matches) {
    auraio_engine_t *engine = make_engine(3, 32);
    assert(engine);

    char buf[262144];
    int len = auraio_metrics_otel(engine, buf, sizeof(buf));
    assert(len > 0);
    buf[len] = '\0';

    /* ring_count gauge should report 3 */
    assert(contains(buf, "\"auraio.ring_count\""));
    assert(contains(buf, "\"asInt\":\"3\""));

    /* Ring attribute "2" should be present (0-indexed) */
    assert(contains(buf, "\"intValue\":\"2\""));

    auraio_destroy(engine);
}

TEST(nonzero_counters_after_io) {
    io_setup();
    auraio_engine_t *engine = make_engine(1, 64);
    assert(engine);

    void *buf = auraio_buffer_alloc(engine, 4096);
    assert(buf);

    for (int i = 0; i < 8; i++) {
        callback_called = 0;
        lseek(test_fd, 0, SEEK_SET);
        auraio_request_t *req =
            auraio_read(engine, test_fd, auraio_buf(buf), 4096, 0, test_callback, NULL);
        assert(req);
        auraio_wait(engine, 1000);
        assert(callback_called == 1);
    }

    char json[131072];
    int len = auraio_metrics_otel(engine, json, sizeof(json));
    assert(len > 0);
    json[len] = '\0';

    /* ops_completed should not be "0" */
    /* Find the ops.completed datapoint and verify non-zero */
    const char *ops = strstr(json, "\"auraio.ops.completed\"");
    assert(ops);
    const char *asint = strstr(ops, "\"asInt\":\"");
    assert(asint);
    asint += strlen("\"asInt\":\"");
    /* Should not start with 0" (i.e., should not be "0") */
    assert(!(asint[0] == '0' && asint[1] == '"'));

    /* bytes.transferred should be non-zero too */
    const char *bytes = strstr(json, "\"auraio.bytes.transferred\"");
    assert(bytes);
    asint = strstr(bytes, "\"asInt\":\"");
    assert(asint);
    asint += strlen("\"asInt\":\"");
    assert(!(asint[0] == '0' && asint[1] == '"'));

    auraio_buffer_free(engine, buf, 4096);
    auraio_destroy(engine);
    io_teardown();
}

TEST(optimal_inflight_positive) {
    auraio_engine_t *engine = make_engine(1, 64);
    assert(engine);

    char buf[131072];
    int len = auraio_metrics_otel(engine, buf, sizeof(buf));
    assert(len > 0);
    buf[len] = '\0';

    /* optimal_in_flight should be > 0 */
    const char *oif = strstr(buf, "\"auraio.optimal_in_flight\"");
    assert(oif);
    const char *asint = strstr(oif, "\"asInt\":\"");
    assert(asint);
    asint += strlen("\"asInt\":\"");
    int val = atoi(asint);
    assert(val > 0);

    auraio_destroy(engine);
}

/* ============================================================================
 * Multi-Ring Tests
 * ============================================================================ */

TEST(multiple_rings_histogram) {
    auraio_engine_t *engine = make_engine(2, 32);
    assert(engine);

    char buf[262144];
    int len = auraio_metrics_otel(engine, buf, sizeof(buf));
    assert(len > 0);
    buf[len] = '\0';

    /* Should have histogram with dataPoints for both rings */
    const char *hist = strstr(buf, "\"auraio.latency\"");
    assert(hist);

    /* Both ring 0 and ring 1 should be in histogram dataPoints */
    const char *dp = strstr(hist, "\"dataPoints\"");
    assert(dp);
    assert(strstr(dp, "\"intValue\":\"0\""));
    assert(strstr(dp, "\"intValue\":\"1\""));

    auraio_destroy(engine);
}

/* ============================================================================
 * Push Helper Tests (URL parsing, no actual network)
 * ============================================================================ */

TEST(push_null_args) {
    assert(auraio_otel_push(NULL, "data", 4) == -1);
    assert(auraio_otel_push("http://localhost:4318/v1/metrics", NULL, 4) == -1);
    assert(auraio_otel_push("http://localhost:4318/v1/metrics", "data", 0) == -1);
}

TEST(push_invalid_scheme) {
    /* https not supported */
    assert(auraio_otel_push("https://localhost:4318/v1/metrics", "{}", 2) == -1);
    /* No scheme */
    assert(auraio_otel_push("localhost:4318/v1/metrics", "{}", 2) == -1);
}

TEST(push_connection_refused) {
    /* Connect to a port that's (almost certainly) not listening */
    int rc = auraio_otel_push("http://127.0.0.1:1/v1/metrics", "{}", 2);
    assert(rc == -1);
}

TEST(push_success_status_and_request_shape) {
    struct push_server_ctx srv;
    assert(push_server_start(&srv, 200) == 0);

    char endpoint[128];
    snprintf(endpoint, sizeof(endpoint), "http://127.0.0.1:%d/v1/metrics", srv.port);

    int rc = auraio_otel_push(endpoint, "{}", 2);
    assert(rc == 200);

    push_server_join(&srv);

    assert(contains(srv.request, "POST /v1/metrics HTTP/1.1\r\n"));
    assert(contains(srv.request, "Content-Type: application/json\r\n"));
    assert(contains(srv.request, "Content-Length: 2\r\n"));
    assert(contains(srv.request, "\r\n\r\n{}"));
}

TEST(push_status_passthrough) {
    struct push_server_ctx srv;
    assert(push_server_start(&srv, 503) == 0);

    char endpoint[128];
    snprintf(endpoint, sizeof(endpoint), "http://127.0.0.1:%d/v1/metrics", srv.port);

    int rc = auraio_otel_push(endpoint, "{}", 2);
    assert(rc == 503);

    push_server_join(&srv);
}

TEST(push_default_path_when_missing) {
    struct push_server_ctx srv;
    assert(push_server_start(&srv, 200) == 0);

    char endpoint[128];
    snprintf(endpoint, sizeof(endpoint), "http://127.0.0.1:%d", srv.port);

    int rc = auraio_otel_push(endpoint, "{}", 2);
    assert(rc == 200);

    push_server_join(&srv);
    assert(contains(srv.request, "POST / HTTP/1.1\r\n"));
}

TEST(push_invalid_authority_forms) {
    /* Empty host */
    assert(auraio_otel_push("http:///v1/metrics", "{}", 2) == -1);
    /* Empty port */
    assert(auraio_otel_push("http://localhost:/v1/metrics", "{}", 2) == -1);
    /* Invalid IPv6 authority (missing closing bracket) */
    assert(auraio_otel_push("http://[::1/v1/metrics", "{}", 2) == -1);
    /* Ambiguous unbracketed IPv6-like authority */
    assert(auraio_otel_push("http://::1:4318/v1/metrics", "{}", 2) == -1);
}

/* ============================================================================
 * Idempotency / Consistency Tests
 * ============================================================================ */

TEST(two_calls_consistent) {
    auraio_engine_t *engine = make_engine(1, 32);
    assert(engine);

    char buf1[131072], buf2[131072];
    int len1 = auraio_metrics_otel(engine, buf1, sizeof(buf1));
    int len2 = auraio_metrics_otel(engine, buf2, sizeof(buf2));
    assert(len1 > 0);
    assert(len2 > 0);

    /* Same engine with no I/O — lengths should be identical.
     * Timestamps differ, but structure and counters match.
     * We just verify the same structure size (timestamp differences
     * don't change length since nanosecond strings are same width). */
    assert(len1 == len2);

    auraio_destroy(engine);
}

/* ============================================================================
 * Main
 * ============================================================================ */

int main(void) {
    atexit(cleanup_atexit);
    printf("\n=== OpenTelemetry Exporter Tests ===\n\n");

    /* NULL / invalid args */
    RUN_TEST(null_engine);
    RUN_TEST(null_buffer);
    RUN_TEST(zero_size_buffer);

    /* Buffer overflow */
    RUN_TEST(tiny_buffer_returns_negative);
    RUN_TEST(retry_with_estimated_size);

    /* JSON structure */
    RUN_TEST(valid_json_braces);
    RUN_TEST(contains_resource_metrics);
    RUN_TEST(contains_schema_version);
    RUN_TEST(contains_library_version);
    RUN_TEST(contains_timestamps);
    RUN_TEST(contains_units);

    /* Metric presence */
    RUN_TEST(contains_aggregate_sums);
    RUN_TEST(contains_aggregate_gauges);
    RUN_TEST(contains_buffer_pool_metrics);
    RUN_TEST(contains_per_ring_metrics);
    RUN_TEST(contains_histogram);

    /* Data values */
    RUN_TEST(ring_count_matches);
    RUN_TEST(optimal_inflight_positive);
    RUN_TEST(nonzero_counters_after_io);

    /* Multi-ring */
    RUN_TEST(multiple_rings_histogram);

    /* Push helper */
    RUN_TEST(push_null_args);
    RUN_TEST(push_invalid_scheme);
    RUN_TEST(push_connection_refused);
    RUN_TEST(push_success_status_and_request_shape);
    RUN_TEST(push_status_passthrough);
    RUN_TEST(push_default_path_when_missing);
    RUN_TEST(push_invalid_authority_forms);

    /* Consistency */
    RUN_TEST(two_calls_consistent);

    printf("\n  All %d tests passed!\n\n", test_count);
    return 0;
}
