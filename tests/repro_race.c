#define _GNU_SOURCE
#include "auraio.h"
#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/eventfd.h>
#include <time.h>
#include <unistd.h>

#define NUM_THREADS 4
#define DURATION_SEC 5

auraio_engine_t *engine;
int efd;
volatile int running = 1;

void on_done(auraio_request_t *req, ssize_t res, void *data) {
  (void)req;
  (void)res;
  (void)data;
}

void *worker(void *arg) {
  uint64_t count = 0;
  uint64_t buf = 1; // eventfd needs 8 bytes

  // Pin thread to a core to ensure it sticks to one ring (mostly)
  // This maximizes contention on that specific ring between this worker and the
  // poller
  int cpu_id = (long)arg % sysconf(_SC_NPROCESSORS_ONLN);
  cpu_set_t cpuset;
  CPU_ZERO(&cpuset);
  CPU_SET(cpu_id, &cpuset);
  pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuset);

  while (running) {
    // Submit write to eventfd
    // We use auraio_buf(&buf) which is UNREGISTERED, so it copies the pointer
    // eventfd writes are very fast, stressing the submission/completion path
    auraio_request_t *req = auraio_write(engine, efd, auraio_buf(&buf),
                                         sizeof(buf), 0, on_done, NULL);
    if (req) {
      count++;
    } else {
      if (errno == EAGAIN || errno == ENOMEM) {
        // Ring full, yield
        sched_yield();
      } else {
        perror("write error");
        break;
      }
    }
  }
  return (void *)count;
}

int main() {
  // Create engine with default options (1 ring per CPU)
  engine = auraio_create();
  if (!engine) {
    perror("create");
    return 1;
  }

  efd = eventfd(0, EFD_NONBLOCK);
  if (efd < 0) {
    perror("eventfd");
    return 1;
  }

  pthread_t threads[NUM_THREADS];
  printf("Starting %d threads for %d seconds...\n", NUM_THREADS, DURATION_SEC);

  for (long i = 0; i < NUM_THREADS; i++) {
    pthread_create(&threads[i], NULL, worker, (void *)i);
  }

  // Main thread acts as the "Reactor" / Poller
  // It iterates all rings and processes completions
  time_t start = time(NULL);
  while (time(NULL) - start < DURATION_SEC) {
    // aggressive polling
    auraio_wait(engine, 10);

    static int print_counter = 0;
    if (print_counter++ % 10 == 0) {
      auraio_stats_t stats;
      auraio_get_stats(engine, &stats);
      printf("\rThroughput: %.2f M/s, P99: %.3f ms, In-flight: %d   ",
             stats.current_throughput_bps / 8.0 / 1000000.0, // 8 bytes per op
             stats.p99_latency_ms, stats.current_in_flight);
      fflush(stdout);
    }
  }
  running = 0;
  printf("\nStopping threads...\n");

  uint64_t total = 0;
  for (int i = 0; i < NUM_THREADS; i++) {
    void *ret;
    pthread_join(threads[i], &ret);
    total += (uint64_t)ret;
  }

  printf("Total submissions: %lu\n", total);
  printf("Average Throughput: %.2f M/s\n",
         (double)total / DURATION_SEC / 1000000.0);

  auraio_destroy(engine);
  close(efd);
  return 0;
}
