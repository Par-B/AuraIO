/**
 * @file quickstart.c
 * @brief Minimal working example of AuraIO async read
 *
 * Build: make examples
 * Run:   ./examples/quickstart
 */

#include <aura.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>

#define BUF_SIZE 4096

static int done = 0;
static ssize_t read_result = 0;

void on_done(aura_request_t *req, ssize_t result, void *user_data) {
    (void)req;
    (void)user_data;
    printf("Read completed: %zd bytes\n", result);
    read_result = result;
    done = 1;
}

int main(void) {
    const char *test_file = "/tmp/aura_quickstart.tmp";
    const char *test_data = "Hello from AuraIO! This is async I/O.\n";

    /* Create a test file with known content */
    int wfd = open(test_file, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (wfd < 0) {
        perror("create test file");
        return 1;
    }
    if (write(wfd, test_data, strlen(test_data)) < 0) {
        perror("write test data");
        close(wfd);
        return 1;
    }
    close(wfd);

    /* Create AuraIO engine */
    aura_engine_t *engine = aura_create();
    if (!engine) {
        perror("aura_create");
        unlink(test_file);
        return 1;
    }

    /* Allocate aligned buffer */
    void *buf = aura_buffer_alloc(engine, BUF_SIZE);
    if (!buf) {
        perror("aura_buffer_alloc");
        aura_destroy(engine);
        unlink(test_file);
        return 1;
    }
    memset(buf, 0, BUF_SIZE);

    /* Open file for reading */
    int fd = open(test_file, O_RDONLY);
    if (fd < 0) {
        perror("open");
        aura_buffer_free(engine, buf);
        aura_destroy(engine);
        unlink(test_file);
        return 1;
    }

    /* Submit async read */
    aura_request_t *req = aura_read(engine, fd, aura_buf(buf), BUF_SIZE, 0, on_done, NULL);
    if (!req) {
        perror("aura_read");
        close(fd);
        aura_buffer_free(engine, buf);
        aura_destroy(engine);
        unlink(test_file);
        return 1;
    }

    /* Wait for completion */
    while (!done) {
        aura_wait(engine, 100);
    }

    /* Verify result */
    if (read_result > 0) {
        printf("Data read: %.*s", (int)read_result, (char *)buf);
    }

    /* Cleanup */
    close(fd);
    aura_buffer_free(engine, buf);
    aura_destroy(engine);
    unlink(test_file);

    printf("Success!\n");
    return 0;
}
