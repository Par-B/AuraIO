/**
 * @file test_request_fd_fix.c
 * @brief Test that aura_request_fd() returns original fd, not internal index
 *
 * Regression test for the bug where aura_request_fd() returned the
 * internal fixed-file table index instead of the original file descriptor
 * when registered files were in use.
 */

#define _POSIX_C_SOURCE 200809L
#include "aura.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdbool.h>

#define TEST_FILE "/tmp/test_request_fd_XXXXXX"
#define BUF_SIZE 4096

static int test_passed = 0;
static int test_failed = 0;

static void pass(const char *name) {
    printf("  %-40s OK\n", name);
    test_passed++;
}

static void fail(const char *name, const char *msg) {
    printf("  %-40s FAIL: %s\n", name, msg);
    test_failed++;
}

/* Test aura_request_fd() without registered files */
static void test_request_fd_no_registered_files(void) {
    const char *test_name = "request_fd_no_registered_files";

    aura_engine_t *engine = aura_create();
    if (!engine) {
        fail(test_name, "Failed to create engine");
        return;
    }

    char template[] = TEST_FILE;
    int fd = mkstemp(template);
    if (fd < 0) {
        fail(test_name, "Failed to create temp file");
        aura_destroy(engine);
        return;
    }

    /* Write some data */
    char buf[BUF_SIZE];
    memset(buf, 'A', BUF_SIZE);
    write(fd, buf, BUF_SIZE);

    /* Submit a read and check aura_request_fd() */
    void *rbuf = aura_buffer_alloc(engine, BUF_SIZE);
    aura_request_t *req = aura_read(engine, fd, aura_buf(rbuf), BUF_SIZE, 0, NULL, NULL);

    if (!req) {
        fail(test_name, "Failed to submit read");
        aura_buffer_free(engine, rbuf);
        close(fd);
        unlink(template);
        aura_destroy(engine);
        return;
    }

    /* Verify aura_request_fd() returns the original fd */
    int returned_fd = aura_request_fd(req);
    if (returned_fd != fd) {
        char msg[128];
        snprintf(msg, sizeof(msg), "Expected fd %d, got %d", fd, returned_fd);
        fail(test_name, msg);
        aura_drain(engine, 1000);
        aura_buffer_free(engine, rbuf);
        close(fd);
        unlink(template);
        aura_destroy(engine);
        return;
    }

    aura_drain(engine, 1000);
    aura_buffer_free(engine, rbuf);
    close(fd);
    unlink(template);
    aura_destroy(engine);
    pass(test_name);
}

/* Test aura_request_fd() WITH registered files (the bug scenario) */
static void test_request_fd_with_registered_files(void) {
    const char *test_name = "request_fd_with_registered_files";

    aura_engine_t *engine = aura_create();
    if (!engine) {
        fail(test_name, "Failed to create engine");
        return;
    }

    /* Create two temp files */
    char template1[] = TEST_FILE;
    char template2[] = TEST_FILE;
    int fd1 = mkstemp(template1);
    int fd2 = mkstemp(template2);

    if (fd1 < 0 || fd2 < 0) {
        fail(test_name, "Failed to create temp files");
        if (fd1 >= 0) close(fd1);
        if (fd2 >= 0) close(fd2);
        aura_destroy(engine);
        return;
    }

    /* Write some data */
    char buf[BUF_SIZE];
    memset(buf, 'A', BUF_SIZE);
    write(fd1, buf, BUF_SIZE);
    write(fd2, buf, BUF_SIZE);

    /* Register the files */
    int fds[] = { fd1, fd2 };
    if (aura_register_files(engine, fds, 2) != 0) {
        fail(test_name, "Failed to register files");
        close(fd1);
        close(fd2);
        unlink(template1);
        unlink(template2);
        aura_destroy(engine);
        return;
    }

    /* Submit reads on both files and verify aura_request_fd() returns original fds */
    void *rbuf1 = aura_buffer_alloc(engine, BUF_SIZE);
    void *rbuf2 = aura_buffer_alloc(engine, BUF_SIZE);

    aura_request_t *req1 = aura_read(engine, fd1, aura_buf(rbuf1), BUF_SIZE, 0, NULL, NULL);
    aura_request_t *req2 = aura_read(engine, fd2, aura_buf(rbuf2), BUF_SIZE, 0, NULL, NULL);

    if (!req1 || !req2) {
        fail(test_name, "Failed to submit reads");
        if (rbuf1) aura_buffer_free(engine, rbuf1);
        if (rbuf2) aura_buffer_free(engine, rbuf2);
        aura_unregister(engine, AURA_REG_FILES);
        close(fd1);
        close(fd2);
        unlink(template1);
        unlink(template2);
        aura_destroy(engine);
        return;
    }

    /* THIS IS THE KEY TEST: aura_request_fd() should return the original fd,
     * NOT the internal fixed-file index (which would be 0 and 1).
     *
     * Before the fix, this would fail because aura_request_fd() would return
     * the index (0 or 1) instead of the actual file descriptor (fd1, fd2).
     */
    int returned_fd1 = aura_request_fd(req1);
    int returned_fd2 = aura_request_fd(req2);

    bool failed = false;
    char msg[256];

    if (returned_fd1 != fd1) {
        snprintf(msg, sizeof(msg),
                 "req1: Expected fd %d, got %d (likely returned index instead of fd)", fd1,
                 returned_fd1);
        fail(test_name, msg);
        failed = true;
    }

    if (!failed && returned_fd2 != fd2) {
        snprintf(msg, sizeof(msg),
                 "req2: Expected fd %d, got %d (likely returned index instead of fd)", fd2,
                 returned_fd2);
        fail(test_name, msg);
        failed = true;
    }

    /* Also verify the fds are not the indices 0 or 1 */
    if (!failed &&
        (returned_fd1 == 0 || returned_fd1 == 1 || returned_fd2 == 0 || returned_fd2 == 1)) {
        snprintf(msg, sizeof(msg), "Returned fd looks like index: fd1=%d, fd2=%d (indices are 0,1)",
                 returned_fd1, returned_fd2);
        fail(test_name, msg);
        failed = true;
    }

    aura_drain(engine, 1000);
    aura_buffer_free(engine, rbuf1);
    aura_buffer_free(engine, rbuf2);
    aura_unregister(engine, AURA_REG_FILES);
    close(fd1);
    close(fd2);
    unlink(template1);
    unlink(template2);
    aura_destroy(engine);

    if (!failed) {
        pass(test_name);
    }
}

/* Test with fsync on registered file */
static void test_request_fd_fsync_registered(void) {
    const char *test_name = "request_fd_fsync_registered";

    aura_engine_t *engine = aura_create();
    if (!engine) {
        fail(test_name, "Failed to create engine");
        return;
    }

    char template[] = TEST_FILE;
    int fd = mkstemp(template);
    if (fd < 0) {
        fail(test_name, "Failed to create temp file");
        aura_destroy(engine);
        return;
    }

    /* Register the file */
    int fds[] = { fd };
    if (aura_register_files(engine, fds, 1) != 0) {
        fail(test_name, "Failed to register file");
        close(fd);
        unlink(template);
        aura_destroy(engine);
        return;
    }

    /* Submit fsync and check aura_request_fd() */
    aura_request_t *req = aura_fsync(engine, fd, AURA_FSYNC_DEFAULT, NULL, NULL);

    if (!req) {
        fail(test_name, "Failed to submit fsync");
        aura_unregister(engine, AURA_REG_FILES);
        close(fd);
        unlink(template);
        aura_destroy(engine);
        return;
    }

    /* Verify aura_request_fd() returns the original fd */
    int returned_fd = aura_request_fd(req);
    if (returned_fd != fd) {
        char msg[128];
        snprintf(msg, sizeof(msg), "Expected fd %d, got %d", fd, returned_fd);
        fail(test_name, msg);
        aura_drain(engine, 1000);
        aura_unregister(engine, AURA_REG_FILES);
        close(fd);
        unlink(template);
        aura_destroy(engine);
        return;
    }

    aura_drain(engine, 1000);
    aura_unregister(engine, AURA_REG_FILES);
    close(fd);
    unlink(template);
    aura_destroy(engine);
    pass(test_name);
}

int main(void) {
    printf("=== aura_request_fd() Fix Tests ===\n");

    test_request_fd_no_registered_files();
    test_request_fd_with_registered_files();
    test_request_fd_fsync_registered();

    printf("\n%d tests passed", test_passed);
    if (test_failed > 0) {
        printf(", %d tests FAILED", test_failed);
    }
    printf("\n");

    return test_failed > 0 ? 1 : 0;
}
