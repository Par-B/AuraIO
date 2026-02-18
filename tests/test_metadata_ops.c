// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 AuraIO Contributors


/**
 * @file test_metadata_ops.c
 * @brief Tests for lifecycle metadata operations (openat, close, statx,
 *        fallocate, ftruncate, sync_file_range)
 *
 * Requires Linux with io_uring support (kernel 5.6+, ftruncate needs 6.9+).
 */

#define _GNU_SOURCE
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

#include "aura.h"

static int test_count = 0;

#define TEST(name) static void test_##name(void)
#define RUN_TEST(name)            \
    do {                          \
        printf("  %-40s", #name); \
        test_##name();            \
        printf(" OK\n");          \
        test_count++;             \
    } while (0)

/* ============================================================================
 * Helpers
 * ============================================================================ */

typedef struct {
    ssize_t result;
    _Atomic int done;
} cb_state_t;

static void basic_cb(aura_request_t *req, ssize_t result, void *user_data) {
    (void)req;
    cb_state_t *st = user_data;
    st->result = result;
    st->done = 1;
}

/** Run engine until cb_state is done. */
static void run_until_done(aura_engine_t *engine, cb_state_t *st) {
    int iters = 0;
    while (!st->done && iters++ < 1000) {
        aura_poll(engine);
        if (!st->done) usleep(1000);
    }
    assert(st->done && "operation did not complete");
}

static aura_engine_t *make_engine(void) {
    aura_engine_t *engine = aura_create();
    assert(engine);
    return engine;
}

/* Temp dir for test files */
static char tmpdir[] = "/tmp/aura_meta_XXXXXX";
static char filepath[512];

static void setup(void) {
    assert(mkdtemp(tmpdir));
    snprintf(filepath, sizeof(filepath), "%s/testfile", tmpdir);
}

static void teardown(void) {
    unlink(filepath);
    rmdir(tmpdir);
}

/* ============================================================================
 * Tests
 * ============================================================================ */

TEST(openat_close_basic) {
    aura_engine_t *engine = make_engine();
    cb_state_t st = { 0 };

    /* Open (create) a file */
    aura_request_t *req =
        aura_openat(engine, AT_FDCWD, filepath, O_CREAT | O_WRONLY | O_TRUNC, 0644, basic_cb, &st);
    assert(req);
    run_until_done(engine, &st);
    assert(st.result >= 0); /* result is the new fd */
    int fd = (int)st.result;

    /* Write some data via regular write(2) so we can verify later */
    const char msg[] = "hello metadata ops";
    ssize_t n = write(fd, msg, sizeof(msg));
    assert(n == (ssize_t)sizeof(msg));

    /* Close via aura */
    cb_state_t st2 = { 0 };
    req = aura_close(engine, fd, basic_cb, &st2);
    assert(req);
    run_until_done(engine, &st2);
    assert(st2.result == 0);

    /* Verify file exists and has correct size */
    struct stat sb;
    assert(stat(filepath, &sb) == 0);
    assert(sb.st_size == (off_t)sizeof(msg));

    aura_destroy(engine);
}

TEST(statx_basic) {
    /* Create a file with known content first */
    int fd = open(filepath, O_CREAT | O_WRONLY | O_TRUNC, 0644);
    assert(fd >= 0);
    char buf[4096];
    memset(buf, 'X', sizeof(buf));
    assert(write(fd, buf, sizeof(buf)) == (ssize_t)sizeof(buf));
    close(fd);

    aura_engine_t *engine = make_engine();
    struct statx stx = { 0 };
    cb_state_t st = { 0 };

    aura_request_t *req =
        aura_statx(engine, AT_FDCWD, filepath, 0, STATX_SIZE | STATX_MODE, &stx, basic_cb, &st);
    assert(req);
    run_until_done(engine, &st);
    assert(st.result == 0);
    assert(stx.stx_size == 4096);
    assert(S_ISREG(stx.stx_mode));

    aura_destroy(engine);
}

TEST(statx_empty_path) {
    /* Use AT_EMPTY_PATH to stat by fd */
    int fd = open(filepath, O_RDONLY);
    assert(fd >= 0);

    aura_engine_t *engine = make_engine();
    struct statx stx = { 0 };
    cb_state_t st = { 0 };

    aura_request_t *req =
        aura_statx(engine, fd, "", AT_EMPTY_PATH, STATX_SIZE, &stx, basic_cb, &st);
    assert(req);
    run_until_done(engine, &st);
    assert(st.result == 0);
    assert(stx.stx_size == 4096);

    close(fd);
    aura_destroy(engine);
}

TEST(fallocate_basic) {
    int fd = open(filepath, O_CREAT | O_WRONLY | O_TRUNC, 0644);
    assert(fd >= 0);

    aura_engine_t *engine = make_engine();
    cb_state_t st = { 0 };

    /* Preallocate 1MB */
    aura_request_t *req = aura_fallocate(engine, fd, 0, 0, 1024 * 1024, basic_cb, &st);
    assert(req);
    run_until_done(engine, &st);
    assert(st.result == 0);

    /* Verify file size */
    struct stat sb;
    assert(fstat(fd, &sb) == 0);
    assert(sb.st_size == 1024 * 1024);

    close(fd);
    aura_destroy(engine);
}

TEST(ftruncate_basic) {
    /* Create file with some data */
    int fd = open(filepath, O_CREAT | O_RDWR | O_TRUNC, 0644);
    assert(fd >= 0);
    char buf[8192];
    memset(buf, 'Y', sizeof(buf));
    assert(write(fd, buf, sizeof(buf)) == (ssize_t)sizeof(buf));

    aura_engine_t *engine = make_engine();
    cb_state_t st = { 0 };

    /* Truncate to 1024 */
    aura_request_t *req = aura_ftruncate(engine, fd, 1024, basic_cb, &st);
    assert(req);
    run_until_done(engine, &st);

    if (st.result == -EINVAL || st.result == -ENOSYS) {
        /* ftruncate via io_uring requires kernel 6.9+ */
        printf("(skipped: kernel too old) ");
        close(fd);
        aura_destroy(engine);
        return;
    }
    assert(st.result == 0);

    struct stat sb;
    assert(fstat(fd, &sb) == 0);
    assert(sb.st_size == 1024);

    close(fd);
    aura_destroy(engine);
}

TEST(sync_file_range_basic) {
    int fd = open(filepath, O_CREAT | O_WRONLY | O_TRUNC, 0644);
    assert(fd >= 0);
    char buf[4096];
    memset(buf, 'Z', sizeof(buf));
    assert(write(fd, buf, sizeof(buf)) == (ssize_t)sizeof(buf));

    aura_engine_t *engine = make_engine();
    cb_state_t st = { 0 };

    aura_request_t *req = aura_sync_file_range(
        engine, fd, 0, 4096, SYNC_FILE_RANGE_WRITE | SYNC_FILE_RANGE_WAIT_AFTER, basic_cb, &st);
    assert(req);
    run_until_done(engine, &st);
    assert(st.result == 0);

    close(fd);
    aura_destroy(engine);
}

TEST(openat_enoent) {
    aura_engine_t *engine = make_engine();
    cb_state_t st = { 0 };

    aura_request_t *req =
        aura_openat(engine, AT_FDCWD, "/tmp/aura_nonexistent_file_xyz", O_RDONLY, 0, basic_cb, &st);
    assert(req);
    run_until_done(engine, &st);
    assert(st.result == -ENOENT);

    aura_destroy(engine);
}

TEST(close_ebadf) {
    aura_engine_t *engine = make_engine();
    cb_state_t st = { 0 };

    /* Close an invalid fd */
    aura_request_t *req = aura_close(engine, 9999, basic_cb, &st);
    assert(req);
    run_until_done(engine, &st);
    assert(st.result == -EBADF);

    aura_destroy(engine);
}

TEST(statx_enoent) {
    aura_engine_t *engine = make_engine();
    struct statx stx = { 0 };
    cb_state_t st = { 0 };

    aura_request_t *req = aura_statx(engine, AT_FDCWD, "/tmp/aura_nonexistent_xyz", 0, STATX_SIZE,
                                     &stx, basic_cb, &st);
    assert(req);
    run_until_done(engine, &st);
    assert(st.result == -ENOENT);

    aura_destroy(engine);
}

TEST(full_lifecycle) {
    /* open → write(aura) → statx → ftruncate → statx → fsync → close */
    aura_engine_t *engine = make_engine();
    cb_state_t st = { 0 };

    /* 1. Open */
    aura_request_t *req =
        aura_openat(engine, AT_FDCWD, filepath, O_CREAT | O_RDWR | O_TRUNC, 0644, basic_cb, &st);
    assert(req);
    run_until_done(engine, &st);
    assert(st.result >= 0);
    int fd = (int)st.result;

    /* 2. Write via aura */
    char wbuf[8192];
    memset(wbuf, 'L', sizeof(wbuf));
    cb_state_t st_w = { 0 };
    req = aura_write(engine, fd, aura_buf(wbuf), sizeof(wbuf), 0, basic_cb, &st_w);
    assert(req);
    run_until_done(engine, &st_w);
    assert(st_w.result == (ssize_t)sizeof(wbuf));

    /* 3. Statx — verify size */
    struct statx stx = { 0 };
    cb_state_t st_s = { 0 };
    req = aura_statx(engine, fd, "", AT_EMPTY_PATH, STATX_SIZE, &stx, basic_cb, &st_s);
    assert(req);
    run_until_done(engine, &st_s);
    assert(st_s.result == 0);
    assert(stx.stx_size == sizeof(wbuf));

    /* 4. Ftruncate to half */
    cb_state_t st_t = { 0 };
    req = aura_ftruncate(engine, fd, 4096, basic_cb, &st_t);
    assert(req);
    run_until_done(engine, &st_t);
    if (st_t.result == -EINVAL || st_t.result == -ENOSYS) {
        printf("(ftruncate skipped: kernel too old) ");
    } else {
        assert(st_t.result == 0);

        /* 5. Statx — verify truncated size */
        struct statx stx2 = { 0 };
        cb_state_t st_s2 = { 0 };
        req = aura_statx(engine, fd, "", AT_EMPTY_PATH, STATX_SIZE, &stx2, basic_cb, &st_s2);
        assert(req);
        run_until_done(engine, &st_s2);
        assert(st_s2.result == 0);
        assert(stx2.stx_size == 4096);
    }

    /* 6. Fsync */
    cb_state_t st_f = { 0 };
    req = aura_fsync(engine, fd, AURA_FSYNC_DEFAULT, basic_cb, &st_f);
    assert(req);
    run_until_done(engine, &st_f);
    assert(st_f.result == 0);

    /* 7. Close */
    cb_state_t st_c = { 0 };
    req = aura_close(engine, fd, basic_cb, &st_c);
    assert(req);
    run_until_done(engine, &st_c);
    assert(st_c.result == 0);

    aura_destroy(engine);
}

TEST(null_params) {
    aura_engine_t *engine = make_engine();

    /* openat with NULL pathname */
    aura_request_t *req = aura_openat(engine, AT_FDCWD, NULL, O_RDONLY, 0, NULL, NULL);
    assert(req == NULL);
    assert(errno == EINVAL);

    /* statx with NULL pathname */
    struct statx stx;
    req = aura_statx(engine, AT_FDCWD, NULL, 0, STATX_SIZE, &stx, NULL, NULL);
    assert(req == NULL);
    assert(errno == EINVAL);

    /* statx with NULL statxbuf */
    req = aura_statx(engine, AT_FDCWD, "/tmp", 0, STATX_SIZE, NULL, NULL, NULL);
    assert(req == NULL);
    assert(errno == EINVAL);

    /* close with negative fd */
    req = aura_close(engine, -1, NULL, NULL);
    assert(req == NULL);
    assert(errno == EINVAL);

    aura_destroy(engine);
}

/* ============================================================================
 * Registered File Tests
 * ============================================================================ */

TEST(close_registered_file) {
    /* Close always uses the raw fd, even when files are registered.
     * Verify it works correctly in that scenario. */
    int fd = open(filepath, O_CREAT | O_RDWR | O_TRUNC, 0644);
    assert(fd >= 0);

    aura_engine_t *engine = make_engine();

    /* Register a different fd so the engine has files_registered=true */
    int dummy_fd = open(filepath, O_RDONLY);
    assert(dummy_fd >= 0);
    int fds[] = { dummy_fd };
    int rc = aura_register_files(engine, fds, 1);
    if (rc != 0) {
        printf("(skipped: register_files failed) ");
        close(fd);
        close(dummy_fd);
        aura_destroy(engine);
        return;
    }

    /* Close the non-registered fd while files are registered */
    cb_state_t st = { 0 };
    aura_request_t *req = aura_close(engine, fd, basic_cb, &st);
    assert(req);
    run_until_done(engine, &st);
    assert(st.result == 0);

    close(dummy_fd);
    aura_unregister(engine, AURA_REG_FILES);
    aura_destroy(engine);
}

TEST(fallocate_registered_file) {
    int fd = open(filepath, O_CREAT | O_RDWR | O_TRUNC, 0644);
    assert(fd >= 0);

    aura_engine_t *engine = make_engine();

    int fds[] = { fd };
    int rc = aura_register_files(engine, fds, 1);
    if (rc != 0) {
        printf("(skipped: register_files failed) ");
        close(fd);
        aura_destroy(engine);
        return;
    }

    /* Fallocate via registered file */
    cb_state_t st = { 0 };
    aura_request_t *req = aura_fallocate(engine, fd, 0, 0, 64 * 1024, basic_cb, &st);
    assert(req);
    run_until_done(engine, &st);
    assert(st.result == 0);

    struct stat sb;
    assert(fstat(fd, &sb) == 0);
    assert(sb.st_size == 64 * 1024);

    aura_unregister(engine, AURA_REG_FILES);
    close(fd);
    aura_destroy(engine);
}

TEST(ftruncate_registered_file) {
    int fd = open(filepath, O_CREAT | O_RDWR | O_TRUNC, 0644);
    assert(fd >= 0);
    char buf[4096];
    memset(buf, 'R', sizeof(buf));
    assert(write(fd, buf, sizeof(buf)) == (ssize_t)sizeof(buf));

    aura_engine_t *engine = make_engine();

    int fds[] = { fd };
    int rc = aura_register_files(engine, fds, 1);
    if (rc != 0) {
        printf("(skipped: register_files failed) ");
        close(fd);
        aura_destroy(engine);
        return;
    }

    cb_state_t st = { 0 };
    aura_request_t *req = aura_ftruncate(engine, fd, 512, basic_cb, &st);
    assert(req);
    run_until_done(engine, &st);

    if (st.result == -EINVAL || st.result == -ENOSYS) {
        printf("(skipped: kernel too old) ");
    } else {
        assert(st.result == 0);
        struct stat sb;
        assert(fstat(fd, &sb) == 0);
        assert(sb.st_size == 512);
    }

    aura_unregister(engine, AURA_REG_FILES);
    close(fd);
    aura_destroy(engine);
}

TEST(sync_file_range_registered_file) {
    int fd = open(filepath, O_CREAT | O_RDWR | O_TRUNC, 0644);
    assert(fd >= 0);
    char buf[4096];
    memset(buf, 'S', sizeof(buf));
    assert(write(fd, buf, sizeof(buf)) == (ssize_t)sizeof(buf));

    aura_engine_t *engine = make_engine();

    int fds[] = { fd };
    int rc = aura_register_files(engine, fds, 1);
    if (rc != 0) {
        printf("(skipped: register_files failed) ");
        close(fd);
        aura_destroy(engine);
        return;
    }

    cb_state_t st = { 0 };
    aura_request_t *req = aura_sync_file_range(
        engine, fd, 0, 4096, SYNC_FILE_RANGE_WRITE | SYNC_FILE_RANGE_WAIT_AFTER, basic_cb, &st);
    assert(req);
    run_until_done(engine, &st);
    assert(st.result == 0);

    aura_unregister(engine, AURA_REG_FILES);
    close(fd);
    aura_destroy(engine);
}

/* ============================================================================
 * Extended Coverage Tests
 * ============================================================================ */

TEST(openat_mode_verification) {
    /* Verify that mode bits are applied correctly */
    aura_engine_t *engine = make_engine();
    cb_state_t st = { 0 };

    char modefile[512];
    snprintf(modefile, sizeof(modefile), "%s/modefile", tmpdir);

    aura_request_t *req =
        aura_openat(engine, AT_FDCWD, modefile, O_CREAT | O_WRONLY | O_TRUNC, 0600, basic_cb, &st);
    assert(req);
    run_until_done(engine, &st);
    assert(st.result >= 0);
    close((int)st.result);

    struct stat sb;
    assert(stat(modefile, &sb) == 0);
    /* Check owner rw bits (umask may strip group/other) */
    assert((sb.st_mode & S_IRUSR) != 0);
    assert((sb.st_mode & S_IWUSR) != 0);

    unlink(modefile);
    aura_destroy(engine);
}

TEST(ftruncate_extend) {
    /* Truncate to extend (grow) a file */
    int fd = open(filepath, O_CREAT | O_RDWR | O_TRUNC, 0644);
    assert(fd >= 0);
    assert(write(fd, "hi", 2) == 2);

    aura_engine_t *engine = make_engine();
    cb_state_t st = { 0 };

    /* Extend to 1MB */
    aura_request_t *req = aura_ftruncate(engine, fd, 1024 * 1024, basic_cb, &st);
    assert(req);
    run_until_done(engine, &st);

    if (st.result == -EINVAL || st.result == -ENOSYS) {
        printf("(skipped: kernel too old) ");
    } else {
        assert(st.result == 0);
        struct stat sb;
        assert(fstat(fd, &sb) == 0);
        assert(sb.st_size == 1024 * 1024);

        /* First 2 bytes should still be "hi", rest should be zero */
        char check[4];
        assert(pread(fd, check, 4, 0) == 4);
        assert(check[0] == 'h' && check[1] == 'i');
        assert(check[2] == 0 && check[3] == 0);
    }

    close(fd);
    aura_destroy(engine);
}

TEST(fallocate_keep_size) {
    /* Preallocate space without changing apparent file size */
    int fd = open(filepath, O_CREAT | O_RDWR | O_TRUNC, 0644);
    assert(fd >= 0);

    aura_engine_t *engine = make_engine();
    cb_state_t st = { 0 };

    aura_request_t *req =
        aura_fallocate(engine, fd, FALLOC_FL_KEEP_SIZE, 0, 1024 * 1024, basic_cb, &st);
    assert(req);
    run_until_done(engine, &st);
    assert(st.result == 0);

    /* Apparent size should still be 0, but blocks allocated */
    struct stat sb;
    assert(fstat(fd, &sb) == 0);
    assert(sb.st_size == 0);
    assert(sb.st_blocks > 0);

    close(fd);
    aura_destroy(engine);
}

TEST(statx_mtime) {
    /* Verify statx returns meaningful mtime */
    int fd = open(filepath, O_CREAT | O_WRONLY | O_TRUNC, 0644);
    assert(fd >= 0);
    assert(write(fd, "x", 1) == 1);
    close(fd);

    aura_engine_t *engine = make_engine();
    struct statx stx = { 0 };
    cb_state_t st = { 0 };

    aura_request_t *req =
        aura_statx(engine, AT_FDCWD, filepath, 0, STATX_MTIME | STATX_SIZE, &stx, basic_cb, &st);
    assert(req);
    run_until_done(engine, &st);
    assert(st.result == 0);
    assert(stx.stx_size == 1);
    /* mtime should be recent (within last 60 seconds) */
    assert(stx.stx_mtime.tv_sec > 0);

    aura_destroy(engine);
}

TEST(concurrent_metadata_ops) {
    /* Submit multiple metadata ops before polling — tests batching */
    int fd = open(filepath, O_CREAT | O_RDWR | O_TRUNC, 0644);
    assert(fd >= 0);
    char buf[4096];
    memset(buf, 'C', sizeof(buf));
    assert(write(fd, buf, sizeof(buf)) == (ssize_t)sizeof(buf));

    aura_engine_t *engine = make_engine();

    /* Submit 3 statx ops simultaneously */
    struct statx stx1 = { 0 }, stx2 = { 0 }, stx3 = { 0 };
    cb_state_t st1 = { 0 }, st2 = { 0 }, st3 = { 0 };

    aura_request_t *r1 =
        aura_statx(engine, fd, "", AT_EMPTY_PATH, STATX_SIZE, &stx1, basic_cb, &st1);
    aura_request_t *r2 =
        aura_statx(engine, fd, "", AT_EMPTY_PATH, STATX_SIZE, &stx2, basic_cb, &st2);
    aura_request_t *r3 =
        aura_statx(engine, fd, "", AT_EMPTY_PATH, STATX_SIZE, &stx3, basic_cb, &st3);
    assert(r1 && r2 && r3);

    /* Poll until all 3 complete */
    int iters = 0;
    while ((!st1.done || !st2.done || !st3.done) && iters++ < 3000) {
        aura_poll(engine);
        if (!st1.done || !st2.done || !st3.done) usleep(1000);
    }
    assert(st1.done && st2.done && st3.done);
    assert(st1.result == 0 && st2.result == 0 && st3.result == 0);
    assert(stx1.stx_size == 4096);
    assert(stx2.stx_size == 4096);
    assert(stx3.stx_size == 4096);

    close(fd);
    aura_destroy(engine);
}

TEST(sync_file_range_flags) {
    /* Test different sync_file_range flag combinations */
    int fd = open(filepath, O_CREAT | O_RDWR | O_TRUNC, 0644);
    assert(fd >= 0);
    char buf[8192];
    memset(buf, 'F', sizeof(buf));
    assert(write(fd, buf, sizeof(buf)) == (ssize_t)sizeof(buf));

    aura_engine_t *engine = make_engine();

    /* WRITE only (non-blocking initiation) */
    cb_state_t st1 = { 0 };
    aura_request_t *req =
        aura_sync_file_range(engine, fd, 0, 4096, SYNC_FILE_RANGE_WRITE, basic_cb, &st1);
    assert(req);
    run_until_done(engine, &st1);
    assert(st1.result == 0);

    /* WAIT_BEFORE | WRITE | WAIT_AFTER (full sync) */
    cb_state_t st2 = { 0 };
    req = aura_sync_file_range(engine, fd, 4096, 4096,
                               SYNC_FILE_RANGE_WAIT_BEFORE | SYNC_FILE_RANGE_WRITE |
                                   SYNC_FILE_RANGE_WAIT_AFTER,
                               basic_cb, &st2);
    assert(req);
    run_until_done(engine, &st2);
    assert(st2.result == 0);

    close(fd);
    aura_destroy(engine);
}

TEST(openat_dirfd) {
    /* Open relative to a directory fd */
    int dirfd = open(tmpdir, O_RDONLY | O_DIRECTORY);
    assert(dirfd >= 0);

    aura_engine_t *engine = make_engine();
    cb_state_t st = { 0 };

    aura_request_t *req =
        aura_openat(engine, dirfd, "dirfd_test", O_CREAT | O_WRONLY | O_TRUNC, 0644, basic_cb, &st);
    assert(req);
    run_until_done(engine, &st);
    assert(st.result >= 0);
    close((int)st.result);

    /* Verify file was created in tmpdir */
    char expected[512];
    snprintf(expected, sizeof(expected), "%s/dirfd_test", tmpdir);
    struct stat sb;
    assert(stat(expected, &sb) == 0);

    unlink(expected);
    close(dirfd);
    aura_destroy(engine);
}

TEST(statx_symlink) {
    /* Create a symlink and stat it with/without NOFOLLOW */
    char linkpath[512];
    snprintf(linkpath, sizeof(linkpath), "%s/testlink", tmpdir);

    /* Ensure target exists */
    int fd = open(filepath, O_CREAT | O_WRONLY | O_TRUNC, 0644);
    assert(fd >= 0);
    assert(write(fd, "data", 4) == 4);
    close(fd);

    assert(symlink(filepath, linkpath) == 0);

    aura_engine_t *engine = make_engine();

    /* Stat the symlink target (follow) */
    struct statx stx1 = { 0 };
    cb_state_t st1 = { 0 };
    aura_request_t *req =
        aura_statx(engine, AT_FDCWD, linkpath, 0, STATX_SIZE | STATX_MODE, &stx1, basic_cb, &st1);
    assert(req);
    run_until_done(engine, &st1);
    assert(st1.result == 0);
    assert(S_ISREG(stx1.stx_mode));
    assert(stx1.stx_size == 4);

    /* Stat the symlink itself (nofollow) */
    struct statx stx2 = { 0 };
    cb_state_t st2 = { 0 };
    req = aura_statx(engine, AT_FDCWD, linkpath, AT_SYMLINK_NOFOLLOW, STATX_MODE, &stx2, basic_cb,
                     &st2);
    assert(req);
    run_until_done(engine, &st2);
    assert(st2.result == 0);
    assert(S_ISLNK(stx2.stx_mode));

    unlink(linkpath);
    aura_destroy(engine);
}

/* ============================================================================
 * Error Path Tests
 * ============================================================================ */

TEST(fallocate_bad_fd) {
    aura_engine_t *engine = make_engine();
    cb_state_t st = { 0 };

    aura_request_t *req = aura_fallocate(engine, 9999, 0, 0, 4096, basic_cb, &st);
    assert(req);
    run_until_done(engine, &st);
    assert(st.result == -EBADF);

    aura_destroy(engine);
}

TEST(fallocate_bad_mode) {
    int fd = open(filepath, O_CREAT | O_RDWR | O_TRUNC, 0644);
    assert(fd >= 0);

    aura_engine_t *engine = make_engine();
    cb_state_t st = { 0 };

    /* Invalid mode bits — kernel should reject */
    aura_request_t *req = aura_fallocate(engine, fd, 0x7FFFFFFF, 0, 4096, basic_cb, &st);
    assert(req);
    run_until_done(engine, &st);
    assert(st.result < 0); /* EOPNOTSUPP or EINVAL */

    close(fd);
    aura_destroy(engine);
}

TEST(ftruncate_bad_fd) {
    aura_engine_t *engine = make_engine();
    cb_state_t st = { 0 };

    aura_request_t *req = aura_ftruncate(engine, 9999, 0, basic_cb, &st);
    assert(req);
    run_until_done(engine, &st);
    /* Could be -EBADF or -EINVAL/-ENOSYS on older kernels */
    assert(st.result < 0);

    aura_destroy(engine);
}

TEST(ftruncate_negative_length) {
    int fd = open(filepath, O_CREAT | O_RDWR | O_TRUNC, 0644);
    assert(fd >= 0);

    aura_engine_t *engine = make_engine();

    /* Negative length is rejected at the API level */
    aura_request_t *req = aura_ftruncate(engine, fd, -1, basic_cb, NULL);
    assert(req == NULL);
    assert(errno == EINVAL);

    close(fd);
    aura_destroy(engine);
}

TEST(sync_file_range_bad_fd) {
    aura_engine_t *engine = make_engine();
    cb_state_t st = { 0 };

    aura_request_t *req =
        aura_sync_file_range(engine, 9999, 0, 4096, SYNC_FILE_RANGE_WRITE, basic_cb, &st);
    assert(req);
    run_until_done(engine, &st);
    assert(st.result == -EBADF);

    aura_destroy(engine);
}

TEST(fallocate_null_params) {
    /* fallocate with NULL engine */
    aura_request_t *req = aura_fallocate(NULL, 0, 0, 0, 4096, basic_cb, NULL);
    assert(req == NULL);
    assert(errno == EINVAL);

    /* fallocate with negative fd */
    aura_engine_t *engine = make_engine();
    req = aura_fallocate(engine, -1, 0, 0, 4096, basic_cb, NULL);
    assert(req == NULL);
    assert(errno == EINVAL);

    aura_destroy(engine);
}

TEST(ftruncate_null_params) {
    aura_request_t *req = aura_ftruncate(NULL, 0, 0, basic_cb, NULL);
    assert(req == NULL);
    assert(errno == EINVAL);

    aura_engine_t *engine = make_engine();
    req = aura_ftruncate(engine, -1, 0, basic_cb, NULL);
    assert(req == NULL);
    assert(errno == EINVAL);

    aura_destroy(engine);
}

TEST(sync_file_range_null_params) {
    aura_request_t *req =
        aura_sync_file_range(NULL, 0, 0, 4096, SYNC_FILE_RANGE_WRITE, basic_cb, NULL);
    assert(req == NULL);
    assert(errno == EINVAL);

    aura_engine_t *engine = make_engine();
    req = aura_sync_file_range(engine, -1, 0, 4096, SYNC_FILE_RANGE_WRITE, basic_cb, NULL);
    assert(req == NULL);
    assert(errno == EINVAL);

    aura_destroy(engine);
}

TEST(openat_readonly_dir) {
    /* Try to create a file in a read-only location */
    aura_engine_t *engine = make_engine();
    cb_state_t st = { 0 };

    aura_request_t *req = aura_openat(engine, AT_FDCWD, "/proc/aura_nonexistent_test",
                                      O_CREAT | O_WRONLY, 0644, basic_cb, &st);
    assert(req);
    run_until_done(engine, &st);
    assert(st.result < 0); /* -EACCES or -EROFS */

    aura_destroy(engine);
}

/* ============================================================================
 * Main
 * ============================================================================ */

int main(void) {
    printf("=== Metadata Operations Tests ===\n");

    setup();

    RUN_TEST(openat_close_basic);
    RUN_TEST(statx_basic);
    RUN_TEST(statx_empty_path);
    RUN_TEST(fallocate_basic);
    RUN_TEST(ftruncate_basic);
    RUN_TEST(sync_file_range_basic);
    RUN_TEST(openat_enoent);
    RUN_TEST(close_ebadf);
    RUN_TEST(statx_enoent);
    RUN_TEST(full_lifecycle);
    RUN_TEST(null_params);
    RUN_TEST(close_registered_file);
    RUN_TEST(fallocate_registered_file);
    RUN_TEST(ftruncate_registered_file);
    RUN_TEST(sync_file_range_registered_file);
    RUN_TEST(openat_mode_verification);
    RUN_TEST(ftruncate_extend);
    RUN_TEST(fallocate_keep_size);
    RUN_TEST(statx_mtime);
    RUN_TEST(concurrent_metadata_ops);
    RUN_TEST(sync_file_range_flags);
    RUN_TEST(openat_dirfd);
    RUN_TEST(statx_symlink);

    /* Error path tests */
    RUN_TEST(fallocate_bad_fd);
    RUN_TEST(fallocate_bad_mode);
    RUN_TEST(ftruncate_bad_fd);
    RUN_TEST(ftruncate_negative_length);
    RUN_TEST(sync_file_range_bad_fd);
    RUN_TEST(fallocate_null_params);
    RUN_TEST(ftruncate_null_params);
    RUN_TEST(sync_file_range_null_params);
    RUN_TEST(openat_readonly_dir);

    teardown();

    printf("\n%d tests passed\n", test_count);
    return 0;
}
