// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 AuraIO Contributors

/**
 * @file atree.c
 * @brief atree - tree replacement with per-file stats powered by AuraIO
 *
 * A tree(1) replacement that shows per-file stats (size, date, permissions)
 * and aggregate summaries per directory. Uses aura_statx to batch stat calls
 * via io_uring for speed on large directory trees.
 *
 * Usage: atree [OPTIONS] [PATH]
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>
#include <inttypes.h>
#include <stdatomic.h>
#include <signal.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
#include <getopt.h>
#include <dirent.h>
#include <pthread.h>
#include <pwd.h>
#include <grp.h>
#include <limits.h>
#include <sched.h>
#include <strings.h>
#include <sys/stat.h>

#include <aura.h>

// ============================================================================
// Constants
// ============================================================================

#define MAX_WORKERS 8
#define STATX_BATCH 64
#define STATX_MASK                                                                            \
    (AURA_STATX_MODE | AURA_STATX_SIZE | AURA_STATX_MTIME | AURA_STATX_UID | AURA_STATX_GID | \
     AURA_STATX_NLINK)

// ============================================================================
// Configuration
// ============================================================================

typedef struct {
    const char *root_path;
    int max_depth; /* -1 = unlimited */
    bool dirs_only;
    bool show_hidden;
    bool sort_by_size;
    bool long_format;
    bool raw_bytes;
    bool use_color;
} config_t;

// ============================================================================
// Tree node
// ============================================================================

typedef struct tree_node {
    char *name;
    char *full_path;
    struct statx st;
    bool is_dir;
    struct tree_node **children;
    size_t num_children;
    size_t cap_children;
    int depth;
    /* Aggregates (dirs only) */
    off_t total_size;
    int64_t total_files;
    int64_t total_dirs;
} tree_node_t;

// ============================================================================
// Visited set for symlink cycle detection (dev/ino hash set)
// ============================================================================

typedef struct {
    dev_t dev;
    ino_t ino;
} devino_t;

typedef struct visited_entry {
    devino_t key;
    bool occupied;
} visited_entry_t;

typedef struct {
    visited_entry_t *buckets;
    size_t cap; /* always a power of 2 */
    size_t count;
    pthread_mutex_t lock;
} visited_set_t;

#define VISITED_INIT_CAP 64
#define VISITED_LOAD_MAX 70 /* percent */

static uint64_t visited_hash(dev_t dev, ino_t ino) {
    /* FNV-1a style mixing */
    uint64_t h = 14695981039346656037ULL;
    h ^= (uint64_t)dev;
    h *= 1099511628211ULL;
    h ^= (uint64_t)ino;
    h *= 1099511628211ULL;
    return h;
}

static bool visited_init(visited_set_t *vs) {
    vs->cap = VISITED_INIT_CAP;
    vs->count = 0;
    vs->buckets = calloc(vs->cap, sizeof(*vs->buckets));
    if (!vs->buckets) {
        vs->cap = 0;
        return false;
    }
    pthread_mutex_init(&vs->lock, NULL);
    return true;
}

static void visited_destroy(visited_set_t *vs) {
    free(vs->buckets);
    pthread_mutex_destroy(&vs->lock);
}

static bool visited_probe(visited_entry_t *buckets, size_t cap, dev_t dev, ino_t ino,
                          size_t *out_idx) {
    size_t mask = cap - 1;
    size_t idx = (size_t)visited_hash(dev, ino) & mask;
    for (;;) {
        if (!buckets[idx].occupied) {
            *out_idx = idx;
            return false; /* empty slot */
        }
        if (buckets[idx].key.dev == dev && buckets[idx].key.ino == ino) {
            *out_idx = idx;
            return true; /* found */
        }
        idx = (idx + 1) & mask;
    }
}

static bool visited_grow(visited_set_t *vs) {
    size_t newcap = vs->cap * 2;
    visited_entry_t *newbuckets = calloc(newcap, sizeof(*newbuckets));
    if (!newbuckets) return false;
    for (size_t i = 0; i < vs->cap; i++) {
        if (vs->buckets[i].occupied) {
            size_t idx;
            visited_probe(newbuckets, newcap, vs->buckets[i].key.dev, vs->buckets[i].key.ino, &idx);
            newbuckets[idx] = vs->buckets[i];
        }
    }
    free(vs->buckets);
    vs->buckets = newbuckets;
    vs->cap = newcap;
    return true;
}

/* Returns true if newly inserted, false if already present. */
static bool visited_insert(visited_set_t *vs, dev_t dev, ino_t ino) {
    pthread_mutex_lock(&vs->lock);
    if (!vs->buckets) {
        pthread_mutex_unlock(&vs->lock);
        return false;
    }
    size_t idx;
    if (visited_probe(vs->buckets, vs->cap, dev, ino, &idx)) {
        pthread_mutex_unlock(&vs->lock);
        return false; /* already present */
    }
    /* Grow if load factor exceeded */
    if (vs->count * 100 / vs->cap >= VISITED_LOAD_MAX) {
        if (!visited_grow(vs)) {
            pthread_mutex_unlock(&vs->lock);
            return false; /* treat as visited on OOM */
        }
        /* Re-probe after grow */
        visited_probe(vs->buckets, vs->cap, dev, ino, &idx);
    }
    vs->buckets[idx].key = (devino_t){ .dev = dev, .ino = ino };
    vs->buckets[idx].occupied = true;
    vs->count++;
    pthread_mutex_unlock(&vs->lock);
    return true;
}

// ============================================================================
// Work queue for parallel directory scanning
// ============================================================================

typedef struct work_item {
    tree_node_t *node;
    struct work_item *next;
} work_item_t;

typedef struct {
    work_item_t *head;
    work_item_t *tail;
    atomic_int pending;
    pthread_mutex_t lock;
    pthread_cond_t cond;
    bool done;
    visited_set_t *visited;
} work_queue_t;

// ============================================================================
// Global state
// ============================================================================

static atomic_int g_interrupted = 0;

static void sigint_handler(int sig) {
    (void)sig;
    atomic_store(&g_interrupted, 1);
}

// ============================================================================
// LS_COLORS parsing
// ============================================================================

typedef struct {
    bool enabled;
    char dir[32];
    char exec[32];
    char link[32];
    char pipe[32];
    char sock[32];
    char reset[8];
} color_scheme_t;

static void parse_ansi_code(const char *code, char *out, size_t outsz) {
    snprintf(out, outsz, "\033[%sm", code);
}

static void color_init(color_scheme_t *cs, bool enabled) {
    memset(cs, 0, sizeof(*cs));
    cs->enabled = enabled;
    if (!enabled) return;

    strcpy(cs->reset, "\033[0m");

    /* Defaults */
    parse_ansi_code("1;34", cs->dir, sizeof(cs->dir));
    parse_ansi_code("1;32", cs->exec, sizeof(cs->exec));
    parse_ansi_code("36", cs->link, sizeof(cs->link));
    parse_ansi_code("33", cs->pipe, sizeof(cs->pipe));
    parse_ansi_code("35", cs->sock, sizeof(cs->sock));

    /* Parse LS_COLORS */
    const char *lsc = getenv("LS_COLORS");
    if (!lsc) return;

    char *copy = strdup(lsc);
    if (!copy) return;

    char *saveptr;
    for (char *tok = strtok_r(copy, ":", &saveptr); tok; tok = strtok_r(NULL, ":", &saveptr)) {
        char *eq = strchr(tok, '=');
        if (!eq) continue;
        *eq = '\0';
        const char *key = tok;
        const char *val = eq + 1;

        if (strcmp(key, "di") == 0) parse_ansi_code(val, cs->dir, sizeof(cs->dir));
        else if (strcmp(key, "ex") == 0) parse_ansi_code(val, cs->exec, sizeof(cs->exec));
        else if (strcmp(key, "ln") == 0) parse_ansi_code(val, cs->link, sizeof(cs->link));
        else if (strcmp(key, "pi") == 0) parse_ansi_code(val, cs->pipe, sizeof(cs->pipe));
        else if (strcmp(key, "so") == 0) parse_ansi_code(val, cs->sock, sizeof(cs->sock));
    }
    free(copy);
}

static const char *color_for_node(const color_scheme_t *cs, const tree_node_t *node) {
    if (!cs->enabled) return "";
    if (node->is_dir) return cs->dir;
    if (S_ISLNK(node->st.stx_mode)) return cs->link;
    if (S_ISFIFO(node->st.stx_mode)) return cs->pipe;
    if (S_ISSOCK(node->st.stx_mode)) return cs->sock;
    if (node->st.stx_mode & (S_IXUSR | S_IXGRP | S_IXOTH)) return cs->exec;
    return "";
}

// ============================================================================
// Formatting helpers
// ============================================================================

static void format_size(char *buf, size_t bufsz, off_t bytes, bool raw) {
    if (raw) {
        snprintf(buf, bufsz, "%lld", (long long)bytes);
        return;
    }
    double b = (double)bytes;
    if (b >= 1024.0 * 1024.0 * 1024.0)
        snprintf(buf, bufsz, "%.1fG", b / (1024.0 * 1024.0 * 1024.0));
    else if (b >= 1024.0 * 1024.0) snprintf(buf, bufsz, "%.1fM", b / (1024.0 * 1024.0));
    else if (b >= 1024.0) snprintf(buf, bufsz, "%.1fK", b / 1024.0);
    else snprintf(buf, bufsz, "%lld", (long long)bytes);
}

static void format_mode(char *buf, uint32_t mode) {
    buf[0] = S_ISDIR(mode)    ? 'd'
             : S_ISLNK(mode)  ? 'l'
             : S_ISBLK(mode)  ? 'b'
             : S_ISCHR(mode)  ? 'c'
             : S_ISFIFO(mode) ? 'p'
             : S_ISSOCK(mode) ? 's'
                              : '-';
    buf[1] = (mode & S_IRUSR) ? 'r' : '-';
    buf[2] = (mode & S_IWUSR) ? 'w' : '-';
    buf[3] = (mode & S_IXUSR) ? ((mode & S_ISUID) ? 's' : 'x') : ((mode & S_ISUID) ? 'S' : '-');
    buf[4] = (mode & S_IRGRP) ? 'r' : '-';
    buf[5] = (mode & S_IWGRP) ? 'w' : '-';
    buf[6] = (mode & S_IXGRP) ? ((mode & S_ISGID) ? 's' : 'x') : ((mode & S_ISGID) ? 'S' : '-');
    buf[7] = (mode & S_IROTH) ? 'r' : '-';
    buf[8] = (mode & S_IWOTH) ? 'w' : '-';
    buf[9] = (mode & S_IXOTH) ? ((mode & S_ISVTX) ? 't' : 'x') : ((mode & S_ISVTX) ? 'T' : '-');
    buf[10] = '\0';
}

static void format_date(char *buf, size_t bufsz, const struct statx_timestamp *ts) {
    time_t t = (time_t)ts->tv_sec;
    struct tm tm;
    localtime_r(&t, &tm);
    strftime(buf, bufsz, "%Y-%m-%d", &tm);
}

// ============================================================================
// Tree node management
// ============================================================================

static tree_node_t *node_create(const char *name, const char *full_path) {
    tree_node_t *n = calloc(1, sizeof(*n));
    if (!n) return NULL;
    n->name = strdup(name);
    n->full_path = strdup(full_path);
    if (!n->name || !n->full_path) {
        free(n->name);
        free(n->full_path);
        free(n);
        return NULL;
    }
    return n;
}

static bool node_add_child(tree_node_t *parent, tree_node_t *child) {
    if (parent->num_children >= parent->cap_children) {
        size_t newcap = parent->cap_children ? parent->cap_children * 2 : 16;
        tree_node_t **tmp = realloc(parent->children, newcap * sizeof(*tmp));
        if (!tmp) return false;
        parent->children = tmp;
        parent->cap_children = newcap;
    }
    parent->children[parent->num_children++] = child;
    return true;
}

static void node_free_recursive(tree_node_t *n, int depth) {
    if (!n) return;
    if (depth < 1000) {
        for (size_t i = 0; i < n->num_children; i++) node_free_recursive(n->children[i], depth + 1);
    }
    /* Children beyond depth 1000 leak to avoid stack overflow */
    free(n->children);
    free(n->name);
    free(n->full_path);
    free(n);
}

static void node_free(tree_node_t *n) {
    if (!n) return;

    /* Iterative post-order traversal using an explicit stack to avoid
       stack overflow on deeply nested directory trees. */
    size_t cap = 256;
    tree_node_t **stack = malloc(cap * sizeof(*stack));
    if (!stack) {
        node_free_recursive(n, 0);
        return;
    }
    size_t sp = 0;
    stack[sp++] = n;

    while (sp > 0) {
        tree_node_t *cur = stack[sp - 1];
        if (cur->num_children > 0) {
            /* Push children, then clear so we don't re-push */
            for (size_t i = 0; i < cur->num_children; i++) {
                if (sp >= cap) {
                    cap *= 2;
                    tree_node_t **tmp = realloc(stack, cap * sizeof(*tmp));
                    if (!tmp) {
                        /* Can't grow stack; skip remaining children (they leak)
                           but keep freeing everything already on the stack. */
                        break;
                    }
                    stack = tmp;
                }
                stack[sp++] = cur->children[i];
            }
            cur->num_children = 0;
        } else {
            sp--;
            free(cur->children);
            free(cur->name);
            free(cur->full_path);
            free(cur);
        }
    }
    free(stack);
}

// ============================================================================
// Statx completion context
// ============================================================================

typedef struct {
    atomic_int remaining;
    atomic_bool abandoned; /* set by caller when bailing on interruption */
} statx_batch_t;

typedef struct {
    tree_node_t *node;
    statx_batch_t *batch;
} statx_ctx_t;

static void on_statx_complete(aura_request_t *req, ssize_t result, void *user_data) {
    (void)req;
    statx_ctx_t *ctx = (statx_ctx_t *)user_data;
    if (result == 0) {
        ctx->node->is_dir = S_ISDIR(ctx->node->st.stx_mode);
    }
    /* If we're the last completion and the caller abandoned us,
       free the shared batch struct. */
    if (atomic_fetch_sub(&ctx->batch->remaining, 1) == 1) {
        if (atomic_load(&ctx->batch->abandoned)) {
            free(ctx->batch);
        }
    }
    free(ctx);
}

// ============================================================================
// Directory scanning with batched statx
// ============================================================================

static int scan_directory(tree_node_t *dir_node, aura_engine_t *engine, const config_t *config,
                          int depth, visited_set_t *visited) {
    /* Skip scanning beyond max_depth — no children to enumerate */
    if (config->max_depth >= 0 && depth >= config->max_depth) return 0;

    /* Cycle detection: skip directories we've already visited (symlink loops) */
    if (visited) {
        struct stat sb;
        if (stat(dir_node->full_path, &sb) == 0) {
            if (!visited_insert(visited, sb.st_dev, sb.st_ino)) {
                return 0; /* already visited */
            }
        }
    }

    DIR *d = opendir(dir_node->full_path);
    if (!d) {
        fprintf(stderr, "atree: cannot open '%s': %s\n", dir_node->full_path, strerror(errno));
        return -1;
    }

    /* First pass: read all entries and create child nodes */
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) continue;
        if (!config->show_hidden && ent->d_name[0] == '.') continue;

        char path[PATH_MAX];
        int plen = snprintf(path, sizeof(path), "%s/%s", dir_node->full_path, ent->d_name);
        if (plen < 0 || (size_t)plen >= sizeof(path)) continue; /* path too long */

        tree_node_t *child = node_create(ent->d_name, path);
        if (!child) continue;
        child->depth = depth + 1;

        /* Quick type hint from dirent to avoid statx for dirs_only filtering later */
        if (ent->d_type == DT_DIR) child->is_dir = true;

        if (!node_add_child(dir_node, child)) {
            node_free(child);
            continue;
        }
    }
    closedir(d);

    if (dir_node->num_children == 0) return 0;

    /* Heap-allocate batch state — shared with async callbacks, so it must
       outlive this function if we bail out of the drain loop early. */
    statx_batch_t *batch = malloc(sizeof(*batch));
    if (!batch) return -1;
    atomic_store(&batch->remaining, (int)dir_node->num_children);
    atomic_store(&batch->abandoned, false);

    for (size_t i = 0; i < dir_node->num_children; i++) {
        tree_node_t *child = dir_node->children[i];

        statx_ctx_t *ctx = malloc(sizeof(*ctx));
        if (!ctx) {
            atomic_fetch_sub(&batch->remaining, 1);
            continue;
        }
        ctx->node = child;
        ctx->batch = batch;

        aura_request_t *req =
            aura_statx(engine, AT_FDCWD, child->full_path, AURA_AT_SYMLINK_NOFOLLOW, STATX_MASK,
                       &child->st, on_statx_complete, ctx);
        if (!req) {
            free(ctx);
            atomic_fetch_sub(&batch->remaining, 1);
            /* Fallback: synchronous statx */
            struct statx stx;
            if (statx(AT_FDCWD, child->full_path, AT_SYMLINK_NOFOLLOW, STATX_MASK, &stx) == 0) {
                child->st = stx;
                child->is_dir = S_ISDIR(stx.stx_mode);
            }
        }
    }

    /* Drain async completions. On interruption, mark the batch as abandoned
       so the last in-flight callback frees it, and bail out immediately. */
    while (atomic_load(&batch->remaining) > 0) {
        if (atomic_load(&g_interrupted)) {
            atomic_store(&batch->abandoned, true);
            return -1;
        }
        aura_wait(engine, 100);
    }

    free(batch);
    return 0;
}

// ============================================================================
// Work queue operations
// ============================================================================

static void wq_init(work_queue_t *wq) {
    memset(wq, 0, sizeof(*wq));
    pthread_mutex_init(&wq->lock, NULL);
    pthread_cond_init(&wq->cond, NULL);
    atomic_store(&wq->pending, 0);
}

static void wq_destroy(work_queue_t *wq) {
    pthread_mutex_destroy(&wq->lock);
    pthread_cond_destroy(&wq->cond);
}

static bool wq_push(work_queue_t *wq, tree_node_t *node) {
    work_item_t *item = malloc(sizeof(*item));
    if (!item) return false;
    item->node = node;
    item->next = NULL;

    pthread_mutex_lock(&wq->lock);
    if (wq->tail) wq->tail->next = item;
    else wq->head = item;
    wq->tail = item;
    pthread_cond_signal(&wq->cond);
    pthread_mutex_unlock(&wq->lock);
    return true;
}

static tree_node_t *wq_pop(work_queue_t *wq) {
    pthread_mutex_lock(&wq->lock);
    while (!wq->head && !wq->done && !atomic_load(&g_interrupted)) {
        /* Use a short timeout so we can poll g_interrupted periodically */
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_nsec += 50 * 1000000; /* 50ms */
        if (ts.tv_nsec >= 1000000000) {
            ts.tv_sec += 1;
            ts.tv_nsec -= 1000000000;
        }
        pthread_cond_timedwait(&wq->cond, &wq->lock, &ts);
    }
    if (!wq->head) {
        pthread_mutex_unlock(&wq->lock);
        return NULL;
    }
    work_item_t *item = wq->head;
    wq->head = item->next;
    if (!wq->head) wq->tail = NULL;
    pthread_mutex_unlock(&wq->lock);

    tree_node_t *node = item->node;
    free(item);
    return node;
}

static void wq_signal_done(work_queue_t *wq) {
    pthread_mutex_lock(&wq->lock);
    wq->done = true;
    pthread_cond_broadcast(&wq->cond);
    pthread_mutex_unlock(&wq->lock);
}

// ============================================================================
// Worker thread
// ============================================================================

typedef struct {
    work_queue_t *wq;
    const config_t *config;
    int worker_id;
} worker_ctx_t;

static void *worker_fn(void *arg) {
    worker_ctx_t *wctx = (worker_ctx_t *)arg;

    aura_options_t opts;
    aura_options_init(&opts);
    opts.queue_depth = STATX_BATCH * 2;
    opts.single_thread = true;
    opts.ring_count = 1;

    aura_engine_t *engine = aura_create_with_options(&opts);
    if (!engine) {
        fprintf(stderr, "atree: worker %d: failed to create engine: %s\n", wctx->worker_id,
                strerror(errno));
        return NULL;
    }

    while (!atomic_load(&g_interrupted)) {
        tree_node_t *node = wq_pop(wctx->wq);
        if (!node) break;

        (void)scan_directory(node, engine, wctx->config, node->depth, wctx->wq->visited);

        /* Enqueue child directories for scanning */
        for (size_t i = 0; i < node->num_children; i++) {
            if (node->children[i]->is_dir) {
                atomic_fetch_add(&wctx->wq->pending, 1);
                if (!wq_push(wctx->wq, node->children[i])) {
                    atomic_fetch_sub(&wctx->wq->pending, 1);
                    fprintf(stderr, "atree: warning: skipping '%s': out of memory\n",
                            node->children[i]->full_path);
                }
            }
        }

        /* Decrement pending; if zero, signal done */
        if (atomic_fetch_sub(&wctx->wq->pending, 1) == 1) {
            wq_signal_done(wctx->wq);
        }
    }

    aura_destroy(engine);
    return NULL;
}

// ============================================================================
// Phase 2: Parallel tree scan
// ============================================================================

static int scan_tree(tree_node_t *root, const config_t *config) {
    visited_set_t visited;
    if (!visited_init(&visited)) {
        fprintf(stderr, "atree: out of memory\n");
        return -1;
    }

    /* Register root directory to prevent cycles back to it */
    {
        struct stat sb;
        if (stat(root->full_path, &sb) == 0) {
            visited_insert(&visited, sb.st_dev, sb.st_ino);
        }
    }

    /* Scan root directory first (single-threaded) */
    aura_options_t opts;
    aura_options_init(&opts);
    opts.queue_depth = STATX_BATCH * 2;
    opts.single_thread = true;
    opts.ring_count = 1;

    aura_engine_t *engine = aura_create_with_options(&opts);
    if (!engine) {
        fprintf(stderr, "atree: failed to create engine: %s\n", strerror(errno));
        visited_destroy(&visited);
        return -1;
    }

    scan_directory(root, engine, config, 0, NULL);
    aura_destroy(engine);

    /* Count child directories to decide on parallelism */
    int child_dirs = 0;
    for (size_t i = 0; i < root->num_children; i++) {
        if (root->children[i]->is_dir) child_dirs++;
    }

    if (child_dirs == 0) {
        visited_destroy(&visited);
        return 0;
    }

    /* Determine worker count */
    long ncpus = sysconf(_SC_NPROCESSORS_ONLN);
    if (ncpus < 1) ncpus = 1;
    if (ncpus > MAX_WORKERS) ncpus = MAX_WORKERS;
    int num_workers = (int)ncpus;
    if (num_workers > child_dirs) num_workers = child_dirs;

    work_queue_t wq;
    wq_init(&wq);
    wq.visited = &visited;

    /* Seed work queue with child directories */
    for (size_t i = 0; i < root->num_children; i++) {
        if (root->children[i]->is_dir) {
            atomic_fetch_add(&wq.pending, 1);
            if (!wq_push(&wq, root->children[i])) {
                atomic_fetch_sub(&wq.pending, 1);
                fprintf(stderr, "atree: warning: skipping '%s': out of memory\n",
                        root->children[i]->full_path);
            }
        }
    }

    /* If no dirs were successfully enqueued, nothing to do */
    if (atomic_load(&wq.pending) == 0) {
        wq_destroy(&wq);
        visited_destroy(&visited);
        return 0;
    }

    /* Spawn workers */
    pthread_t threads[MAX_WORKERS];
    worker_ctx_t wctxs[MAX_WORKERS];

    for (int i = 0; i < num_workers; i++) {
        wctxs[i].wq = &wq;
        wctxs[i].config = config;
        wctxs[i].worker_id = i;
        int rc = pthread_create(&threads[i], NULL, worker_fn, &wctxs[i]);
        if (rc != 0) {
            fprintf(stderr, "atree: pthread_create failed: %s\n", strerror(rc));
            num_workers = i;
            break;
        }
    }

    for (int i = 0; i < num_workers; i++) {
        pthread_join(threads[i], NULL);
    }

    wq_destroy(&wq);
    visited_destroy(&visited);
    return 0;
}

// ============================================================================
// Phase 3: Aggregate (bottom-up)
// ============================================================================

typedef struct {
    tree_node_t *node;
    ssize_t child_idx; /* next child to process; -1 = not yet initialized */
} agg_frame_t;

static void aggregate(tree_node_t *root) {
    size_t cap = 256;
    agg_frame_t *stack = malloc(cap * sizeof(*stack));
    if (!stack) return;
    size_t sp = 0;
    stack[sp++] = (agg_frame_t){ .node = root, .child_idx = -1 };

    while (sp > 0) {
        agg_frame_t *frame = &stack[sp - 1];
        tree_node_t *node = frame->node;

        if (!node->is_dir) {
            node->total_size = (off_t)node->st.stx_size;
            sp--;
            continue;
        }

        if (frame->child_idx == -1) {
            /* Initialize aggregates */
            node->total_size = 0;
            node->total_files = 0;
            node->total_dirs = 0;
            frame->child_idx = 0;
        }

        if ((size_t)frame->child_idx < node->num_children) {
            tree_node_t *c = node->children[(size_t)frame->child_idx];
            frame->child_idx++;

            /* If child already processed (leaf or previously visited), accumulate */
            if (!c->is_dir) {
                c->total_size = (off_t)c->st.stx_size;
                node->total_files++;
                node->total_size += c->total_size;
            } else {
                /* Push child for processing */
                if (sp >= cap) {
                    cap *= 2;
                    agg_frame_t *tmp = realloc(stack, cap * sizeof(*tmp));
                    if (!tmp) {
                        free(stack);
                        return;
                    }
                    stack = tmp;
                    frame = &stack[sp - 1]; /* realloc may move */
                }
                stack[sp++] = (agg_frame_t){ .node = c, .child_idx = -1 };
            }
        } else {
            /* All children processed — accumulate dir children */
            for (size_t i = 0; i < node->num_children; i++) {
                tree_node_t *c = node->children[i];
                if (c->is_dir) {
                    node->total_dirs += 1 + c->total_dirs;
                    node->total_files += c->total_files;
                    node->total_size += c->total_size;
                }
            }
            sp--;
        }
    }
    free(stack);
}

// ============================================================================
// Sorting
// ============================================================================

static int cmp_alpha(const void *a, const void *b) {
    const tree_node_t *na = *(const tree_node_t **)a;
    const tree_node_t *nb = *(const tree_node_t **)b;
    /* Directories first, then alphabetical */
    if (na->is_dir != nb->is_dir) return nb->is_dir - na->is_dir;
    return strcasecmp(na->name, nb->name);
}

static int cmp_size(const void *a, const void *b) {
    const tree_node_t *na = *(const tree_node_t **)a;
    const tree_node_t *nb = *(const tree_node_t **)b;
    /* Directories first, then largest first */
    if (na->is_dir != nb->is_dir) return nb->is_dir - na->is_dir;
    off_t sa = na->is_dir ? na->total_size : (off_t)na->st.stx_size;
    off_t sb = nb->is_dir ? nb->total_size : (off_t)nb->st.stx_size;
    if (sb > sa) return 1;
    if (sb < sa) return -1;
    return strcasecmp(na->name, nb->name);
}

static void sort_tree(tree_node_t *root, bool by_size) {
    size_t cap = 256;
    tree_node_t **stack = malloc(cap * sizeof(*stack));
    if (!stack) return;
    size_t sp = 0;
    stack[sp++] = root;

    while (sp > 0) {
        tree_node_t *node = stack[--sp];
        if (!node->is_dir || node->num_children == 0) continue;

        qsort(node->children, node->num_children, sizeof(tree_node_t *),
              by_size ? cmp_size : cmp_alpha);

        for (size_t i = 0; i < node->num_children; i++) {
            if (node->children[i]->is_dir && node->children[i]->num_children > 0) {
                if (sp >= cap) {
                    cap *= 2;
                    tree_node_t **tmp = realloc(stack, cap * sizeof(*tmp));
                    if (!tmp) {
                        free(stack);
                        return;
                    }
                    stack = tmp;
                }
                stack[sp++] = node->children[i];
            }
        }
    }
    free(stack);
}

// ============================================================================
// Phase 4: Print tree
// ============================================================================

/* Column widths for alignment */
#define NAME_COL 36
#define LONG_COL 40

static void emit_node(const tree_node_t *node, const config_t *config, const color_scheme_t *cs,
                      const char *prefix, bool is_last, int depth, int *file_count,
                      int *dir_count) {
    if (config->dirs_only && !node->is_dir) return;

    /* Build the tree connector + name part using dynamic buffer */
    size_t prefix_len = prefix ? strlen(prefix) : 0;
    size_t name_len = strlen(node->name);
    /* Worst case: prefix + connector(10) + color(16) + name + "/" + reset(8) + NUL */
    size_t line_sz = prefix_len + name_len + 64;
    char *line = malloc(line_sz);
    if (!line) return;

    if (depth == 0) {
        const char *clr = color_for_node(cs, node);
        const char *rst = cs->enabled ? cs->reset : "";
        snprintf(line, line_sz, "%s%s/%s", clr, node->name, rst);
    } else {
        const char *connector = is_last ? "\xe2\x94\x94\xe2\x94\x80\xe2\x94\x80 "
                                        : "\xe2\x94\x9c\xe2\x94\x80\xe2\x94\x80 ";
        const char *clr = color_for_node(cs, node);
        const char *rst = cs->enabled ? cs->reset : "";
        if (node->is_dir)
            snprintf(line, line_sz, "%s%s%s%s/%s", prefix, connector, clr, node->name, rst);
        else snprintf(line, line_sz, "%s%s%s%s%s", prefix, connector, clr, node->name, rst);
    }

    /* Calculate visible length (without ANSI escapes) for padding */
    int visible_len = 0;
    if (depth == 0) {
        visible_len = (int)name_len + 1;
    } else {
        visible_len = depth * 4 + (int)name_len + (node->is_dir ? 1 : 0);
    }

    /* Right-aligned stats */
    char stats[256];

    if (config->long_format) {
        char mode_str[12];
        format_mode(mode_str, node->st.stx_mode);

        struct passwd pw_buf, *pw_result;
        struct group gr_buf, *gr_result;
        char pw_str[1024], gr_str[1024];
        getpwuid_r(node->st.stx_uid, &pw_buf, pw_str, sizeof(pw_str), &pw_result);
        getgrgid_r(node->st.stx_gid, &gr_buf, gr_str, sizeof(gr_str), &gr_result);
        const char *uname = pw_result ? pw_result->pw_name : "?";
        const char *gname = gr_result ? gr_result->gr_name : "?";

        if (node->is_dir) {
            char sz[32];
            format_size(sz, sizeof(sz), node->total_size, config->raw_bytes);
            int64_t fc = node->total_files;
            int64_t dc = node->total_dirs;
            if (dc > 0) {
                snprintf(stats, sizeof(stats),
                         "%s  %-8s %-8s  [%" PRId64 " file%s, %" PRId64 " dir%s, %s]", mode_str,
                         uname, gname, fc, fc == 1 ? "" : "s", dc, dc == 1 ? "" : "s", sz);
            } else {
                snprintf(stats, sizeof(stats), "%s  %-8s %-8s  [%" PRId64 " file%s, %s]", mode_str,
                         uname, gname, fc, fc == 1 ? "" : "s", sz);
            }
        } else {
            char sz[32], date[16];
            format_size(sz, sizeof(sz), (off_t)node->st.stx_size, config->raw_bytes);
            format_date(date, sizeof(date), &node->st.stx_mtime);
            snprintf(stats, sizeof(stats), "%s  %-8s %-8s  %7s  %s", mode_str, uname, gname, sz,
                     date);
        }
    } else {
        if (node->is_dir) {
            char sz[32];
            format_size(sz, sizeof(sz), node->total_size, config->raw_bytes);
            int64_t fc = node->total_files;
            int64_t dc = node->total_dirs;
            if (dc > 0) {
                snprintf(stats, sizeof(stats), "[%" PRId64 " file%s, %" PRId64 " dir%s, %s]", fc,
                         fc == 1 ? "" : "s", dc, dc == 1 ? "" : "s", sz);
            } else {
                snprintf(stats, sizeof(stats), "[%" PRId64 " file%s, %s]", fc, fc == 1 ? "" : "s",
                         sz);
            }
        } else {
            char sz[32], date[16];
            format_size(sz, sizeof(sz), (off_t)node->st.stx_size, config->raw_bytes);
            format_date(date, sizeof(date), &node->st.stx_mtime);
            snprintf(stats, sizeof(stats), "%7s  %s", sz, date);
        }
    }

    int target_col = config->long_format ? LONG_COL : NAME_COL;
    int pad = target_col - visible_len;
    if (pad < 2) pad = 2;

    printf("%s", line);
    for (int i = 0; i < pad; i++) putchar(' ');
    printf("%s\n", stats);
    free(line);

    if (node->is_dir && depth > 0) (*dir_count)++;
    else if (!node->is_dir) (*file_count)++;
}

/* Iterative pre-order tree printer to avoid stack overflow on deep trees. */
typedef struct {
    const tree_node_t *node;
    char *prefix; /* heap-allocated prefix for children */
    bool is_last;
    int depth;
    ssize_t child_idx; /* next visible child to process; -1 = node not yet printed */
} print_frame_t;

static void print_node(const tree_node_t *root, const config_t *config, const color_scheme_t *cs,
                       const char *prefix, bool is_last, int depth, int *file_count,
                       int *dir_count) {
    size_t cap = 256;
    print_frame_t *stack = malloc(cap * sizeof(*stack));
    if (!stack) return;
    size_t sp = 0;

    stack[sp++] = (print_frame_t){ .node = root,
                                   .prefix = strdup(prefix ? prefix : ""),
                                   .is_last = is_last,
                                   .depth = depth,
                                   .child_idx = -1 };

    while (sp > 0) {
        print_frame_t *frame = &stack[sp - 1];
        const tree_node_t *node = frame->node;

        if (config->max_depth >= 0 && frame->depth > config->max_depth) {
            free(frame->prefix);
            sp--;
            continue;
        }

        /* First visit: print the node itself */
        if (frame->child_idx == -1) {
            emit_node(node, config, cs, frame->prefix, frame->is_last, frame->depth, file_count,
                      dir_count);
            frame->child_idx = 0;
        }

        /* If not a dir or at max_depth, we're done */
        if (!node->is_dir || (config->max_depth >= 0 && frame->depth >= config->max_depth)) {
            free(frame->prefix);
            sp--;
            continue;
        }

        /* Find next visible child */
        size_t ci = (size_t)frame->child_idx; /* safe: child_idx >= 0 here */
        while (ci < node->num_children) {
            if (!config->dirs_only || node->children[ci]->is_dir) break;
            ci++;
        }

        if (ci >= node->num_children) {
            /* No more children */
            free(frame->prefix);
            sp--;
            continue;
        }

        frame->child_idx = (ssize_t)(ci + 1);

        /* Determine if this child is the last visible one */
        bool child_is_last = true;
        for (size_t j = ci + 1; j < node->num_children; j++) {
            if (!config->dirs_only || node->children[j]->is_dir) {
                child_is_last = false;
                break;
            }
        }

        /* Build child prefix */
        char *child_prefix;
        if (frame->depth == 0) {
            child_prefix = strdup("");
        } else {
            const char *ext = frame->is_last ? "    " : "\xe2\x94\x82   ";
            size_t plen = strlen(frame->prefix) + strlen(ext) + 1;
            child_prefix = malloc(plen);
            if (child_prefix) snprintf(child_prefix, plen, "%s%s", frame->prefix, ext);
        }

        if (!child_prefix) continue;

        /* Push child frame */
        if (sp >= cap) {
            cap *= 2;
            print_frame_t *tmp = realloc(stack, cap * sizeof(*tmp));
            if (!tmp) {
                /* Can't grow stack; free leaked prefixes and bail */
                free(child_prefix);
                for (size_t k = 0; k < sp; k++) {
                    free(stack[k].prefix);
                }
                free(stack);
                return;
            }
            stack = tmp;
            frame = &stack[sp - 1];
        }
        stack[sp++] = (print_frame_t){ .node = node->children[ci],
                                       .prefix = child_prefix,
                                       .is_last = child_is_last,
                                       .depth = frame->depth + 1,
                                       .child_idx = -1 };
    }
    free(stack);
}

// ============================================================================
// CLI parsing
// ============================================================================

static void print_usage(const char *argv0) {
    fprintf(stderr,
            "Usage: %s [OPTIONS] [PATH]\n"
            "\n"
            "Display directory tree with per-file stats powered by AuraIO.\n"
            "\n"
            "Options:\n"
            "  -L <depth>    Max display depth (default: unlimited)\n"
            "  -d            Directories only\n"
            "  -a            Show hidden files\n"
            "  -s            Sort by size (default: alphabetical)\n"
            "  -l            Long format (permissions, owner, group)\n"
            "  -b            Show raw byte counts\n"
            "  --no-color    Disable color output\n"
            "  -h, --help    Show this help\n",
            argv0);
}

static int parse_args(int argc, char **argv, config_t *config) {
    memset(config, 0, sizeof(*config));
    config->max_depth = -1;
    config->use_color = isatty(STDOUT_FILENO);

    static struct option long_opts[] = { { "no-color", no_argument, 0, 'C' },
                                         { "help", no_argument, 0, 'h' },
                                         { 0, 0, 0, 0 } };

    int opt;
    while ((opt = getopt_long(argc, argv, "L:daslbh", long_opts, NULL)) != -1) {
        switch (opt) {
        case 'L': {
            char *end;
            errno = 0;
            long val = strtol(optarg, &end, 10);
            if (*end != '\0' || val < 0 || val > INT_MAX || errno == ERANGE) {
                fprintf(stderr, "atree: invalid depth: %s\n", optarg);
                return -1;
            }
            config->max_depth = (int)val;
            break;
        }
        case 'd':
            config->dirs_only = true;
            break;
        case 'a':
            config->show_hidden = true;
            break;
        case 's':
            config->sort_by_size = true;
            break;
        case 'l':
            config->long_format = true;
            break;
        case 'b':
            config->raw_bytes = true;
            break;
        case 'C':
            config->use_color = false;
            break;
        case 'h':
            print_usage(argv[0]);
            exit(0);
        default:
            print_usage(argv[0]);
            return -1;
        }
    }

    if (optind < argc) config->root_path = argv[optind];
    else config->root_path = ".";

    return 0;
}

// ============================================================================
// Main
// ============================================================================

int main(int argc, char **argv) {
    config_t config;
    if (parse_args(argc, argv, &config) != 0) return 1;

    /* Install SIGINT handler */
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = sigint_handler;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, NULL);

    struct timespec t_start;
    clock_gettime(CLOCK_MONOTONIC, &t_start);

    /* Stat the root */
    struct statx root_st;
    if (statx(AT_FDCWD, config.root_path, 0, STATX_MASK, &root_st) != 0) {
        fprintf(stderr, "atree: cannot stat '%s': %s\n", config.root_path, strerror(errno));
        return 1;
    }

    if (!S_ISDIR(root_st.stx_mode)) {
        fprintf(stderr, "atree: '%s' is not a directory\n", config.root_path);
        return 1;
    }

    /* Build tree — resolve display name to canonical path */
    char resolved[PATH_MAX];
    const char *display_name = config.root_path;
    if (realpath(config.root_path, resolved)) display_name = resolved;

    tree_node_t *root = node_create(display_name, config.root_path);
    if (!root) return 1;
    root->st = root_st;
    root->is_dir = true;

    if (scan_tree(root, &config) != 0) {
        node_free(root);
        return 1;
    }

    if (atomic_load(&g_interrupted)) {
        node_free(root);
        fprintf(stderr, "\natree: interrupted\n");
        return 130;
    }

    /* Aggregate */
    aggregate(root);

    /* Sort */
    sort_tree(root, config.sort_by_size);

    /* Stop timer before printing (measure scan + aggregate only) */
    struct timespec t_end;
    clock_gettime(CLOCK_MONOTONIC, &t_end);
    double elapsed =
        (double)(t_end.tv_sec - t_start.tv_sec) + (double)(t_end.tv_nsec - t_start.tv_nsec) / 1e9;

    /* Print */
    color_scheme_t cs;
    color_init(&cs, config.use_color);

    int file_count = 0, dir_count = 0;
    print_node(root, &config, &cs, "", true, 0, &file_count, &dir_count);

    char total_sz[32];
    format_size(total_sz, sizeof(total_sz), root->total_size, config.raw_bytes);

    printf("\n%d file%s, %d director%s, %s total  (scanned in %.3fs)\n", file_count,
           file_count == 1 ? "" : "s", dir_count + 1, dir_count == 0 ? "y" : "ies", total_sz,
           elapsed);

    node_free(root);
    return 0;
}
