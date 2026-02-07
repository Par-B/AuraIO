# AuraIO Tests Makefile

CC = gcc
CXX = g++
CFLAGS = -Wall -Wextra -Wpedantic -std=c11 -g -I../include -I../src
CXXFLAGS = -Wall -Wextra -Wshadow -Wpedantic -std=c++20 -g -I../include -fcoroutines
LDFLAGS = -L../lib -lauraio -luring -lpthread
RPATH = -Wl,-rpath,'$$ORIGIN/../lib'

# Sanitizer flags
TSAN_FLAGS = -fsanitize=thread -fPIE -pie
ASAN_FLAGS = -fsanitize=address -fno-omit-frame-pointer

TESTS = test_engine test_buffer test_ring test_stats test_ring_select test_concurrency_stress
CPP_TESTS = test_cpp

.PHONY: all build-tests

all:
	@rm -f $(TESTS) $(CPP_TESTS)
	@$(MAKE) --no-print-directory build-tests
	@echo "Running all C tests..."
	@total=0; \
	for t in $(TESTS); do \
		echo ""; \
		echo "=== $$t ==="; \
		output=$$(./$$t) || exit 1; \
		echo "$$output"; \
		count=$$(echo "$$output" | grep -oE '^[0-9]+ tests passed' | grep -oE '^[0-9]+'); \
		if [ -n "$$count" ]; then total=$$((total + count)); fi; \
	done; \
	echo ""; \
	echo "=== C Tests Summary ==="; \
	echo "$$total tests passed"
	@echo ""
	@echo "Running C++ tests..."
	@for t in $(CPP_TESTS); do \
		echo ""; \
		echo "=== $$t ==="; \
		./$$t || exit 1; \
	done

build-tests: $(TESTS) $(CPP_TESTS)

test_engine: test_engine.c ../lib/libauraio.a
	$(CC) $(CFLAGS) $< -o $@ ../lib/libauraio.a $(LDFLAGS) $(RPATH)

test_buffer: test_buffer.c ../lib/libauraio.a
	$(CC) $(CFLAGS) $< -o $@ ../lib/libauraio.a $(LDFLAGS) $(RPATH)

test_ring: test_ring.c ../lib/libauraio.a
	$(CC) $(CFLAGS) $< -o $@ ../lib/libauraio.a $(LDFLAGS) $(RPATH)

test_stats: test_stats.c ../lib/libauraio.a
	$(CC) $(CFLAGS) $< -o $@ ../lib/libauraio.a $(LDFLAGS) $(RPATH)

test_concurrency_stress: test_concurrency_stress.c ../lib/libauraio.a
	$(CC) $(CFLAGS) $< -o $@ ../lib/libauraio.a $(LDFLAGS) $(RPATH)

# ============================================================================
# C++ Tests
# ============================================================================

test_cpp: test_cpp.cpp ../lib/libauraio.so
	$(CXX) $(CXXFLAGS) $< -o $@ $(LDFLAGS) $(RPATH)

cpp-test: test_cpp
	./test_cpp

# ============================================================================
# Ring Selection Test Utility
# ============================================================================

test_ring_select: test_ring_select.c ../lib/libauraio.a
	$(CC) $(CFLAGS) -O2 $< -o $@ ../lib/libauraio.a $(LDFLAGS) $(RPATH)

ring-select: test_ring_select
	./test_ring_select

# ============================================================================
# Stress Test
# ============================================================================

stress_test: stress_test.cpp ../lib/libauraio.so
	$(CXX) $(CXXFLAGS) -O2 $< -o $@ $(LDFLAGS) $(RPATH)

stress: stress_test
	./stress_test --quick

stress-full: stress_test
	./stress_test --duration 30

# ============================================================================
# Performance Benchmark (for perf profiling)
# ============================================================================

perf_bench: perf_bench.c ../lib/libauraio.a
	$(CC) $(CFLAGS) -O2 $< -o $@ ../lib/libauraio.a $(LDFLAGS) $(RPATH)

bench: perf_bench
	./perf_bench all 5

bench-throughput: perf_bench
	./perf_bench throughput 10

bench-latency: perf_bench
	./perf_bench latency 10

bench-scalability: perf_bench
	./perf_bench scalability 3

# ============================================================================
# Portable Performance Test Suite (with fio baseline)
# ============================================================================

.PHONY: check-deps perf-test perf-test-quick perf-test-full perf-baseline

# Check benchmark dependencies
check-deps:
	@echo "Checking benchmark dependencies..."
	@echo ""
	@command -v fio > /dev/null 2>&1 && echo "[OK] fio: $$(fio --version)" || echo "[MISSING] fio - install with: apt install fio"
	@command -v perf > /dev/null 2>&1 && echo "[OK] perf: $$(perf --version 2>&1 | head -1)" || echo "[OPTIONAL] perf - install with: apt install linux-tools-common linux-tools-$$(uname -r)"
	@command -v numactl > /dev/null 2>&1 && echo "[OK] numactl" || echo "[OPTIONAL] numactl - install with: apt install numactl"
	@command -v iostat > /dev/null 2>&1 && echo "[OK] iostat" || echo "[OPTIONAL] iostat - install with: apt install sysstat"
	@echo ""
	@test -e /proc/sys/kernel/io_uring_disabled && \
		(test "$$(cat /proc/sys/kernel/io_uring_disabled)" = "0" && echo "[OK] io_uring: enabled" || echo "[ERROR] io_uring: disabled") || \
		echo "[OK] io_uring: likely supported (kernel $$(uname -r))"

# Quick performance test (5 sec per test)
perf-test-quick: perf_bench
	./run_perf_test.sh --quick --compare

# Full performance test (30 sec per test)
perf-test-full: perf_bench
	./run_perf_test.sh --full --compare

# Standard performance test (10 sec per test)
perf-test: perf_bench
	./run_perf_test.sh --compare

# Run fio baseline only
perf-baseline:
	./run_perf_test.sh --baseline

# Run AuraIO benchmark with perf profiling
perf-profile: perf_bench
	./run_perf_test.sh --auraio --with-perf

# ============================================================================
# Performance Analysis (laptop-safe, with FIO comparison + report)
# ============================================================================

# Quick analysis (3s per test) - fast sanity check
analyze-quick: perf_bench
	./run_analysis.sh --quick

# Standard analysis (5s per test) - good for laptops
analyze: perf_bench
	./run_analysis.sh --standard

# Full analysis (10s per test) - more stable numbers
analyze-full: perf_bench
	./run_analysis.sh --full

# AuraIO-only analysis (no FIO baseline)
analyze-no-fio: perf_bench
	./run_analysis.sh --skip-fio

# ============================================================================
# Valgrind memory tests
# ============================================================================
valgrind: $(TESTS)
	@echo "Running valgrind memory tests..."
	@for t in $(TESTS); do \
		echo ""; \
		echo "=== valgrind $$t ==="; \
		valgrind --leak-check=full --show-leak-kinds=all --track-origins=yes \
			--error-exitcode=1 ./$$t || exit 1; \
	done
	@echo ""
	@echo "All valgrind tests passed!"

# Quick valgrind (less verbose)
valgrind-quick: $(TESTS)
	@echo "Running quick valgrind tests..."
	@for t in $(TESTS); do \
		echo "=== $$t ==="; \
		valgrind --leak-check=summary --error-exitcode=1 ./$$t 2>&1; \
		status=$$?; \
		if [ $$status -ne 0 ]; then echo "FAIL: $$t (valgrind exit $$status)"; exit 1; fi; \
	done
	@echo "All quick valgrind tests passed!"

# ============================================================================
# ThreadSanitizer tests (requires rebuild with TSAN)
# ============================================================================
TSAN_TESTS = test_engine_tsan test_buffer_tsan test_ring_tsan test_stats_tsan test_ring_select_tsan test_concurrency_stress_tsan

tsan: $(TSAN_TESTS)
	@echo "Running ThreadSanitizer tests..."
	@# TSan crashes on kernels 6.5+ with high ASLR entropy (mmap_rnd_bits > 30).
	@# Use setarch to disable ASLR when needed.
	@TSAN_WRAP=""; \
	NEED_WRAP=0; \
	RND=$$(cat /proc/sys/vm/mmap_rnd_bits 2>/dev/null); \
	if [ -n "$$RND" ] && [ "$$RND" -gt 30 ] 2>/dev/null; then \
		NEED_WRAP=1; \
	elif [ -z "$$RND" ]; then \
		KMAJOR=$$(uname -r | cut -d. -f1); \
		KMINOR=$$(uname -r | cut -d. -f2); \
		if [ "$$KMAJOR" -gt 6 ] 2>/dev/null || \
		   { [ "$$KMAJOR" -eq 6 ] && [ "$$KMINOR" -ge 5 ]; } 2>/dev/null; then \
			NEED_WRAP=1; \
		fi; \
	fi; \
	if [ "$$NEED_WRAP" -eq 1 ]; then \
		if command -v setarch >/dev/null 2>&1; then \
			TSAN_WRAP="setarch $$(uname -m) --addr-no-randomize"; \
			echo "(using setarch to reduce ASLR for TSan compatibility)"; \
		else \
			echo "WARNING: high ASLR entropy detected, TSan may crash."; \
			echo "  Fix: sudo sysctl vm.mmap_rnd_bits=28"; \
		fi; \
	fi; \
	for t in $(TSAN_TESTS); do \
		echo ""; \
		echo "=== $$t ==="; \
		$$TSAN_WRAP ./$$t || exit 1; \
	done; \
	echo ""; \
	echo "All TSAN tests passed!"

test_engine_tsan: test_engine.c ../lib/libauraio_tsan.a
	$(CC) $(CFLAGS) $(TSAN_FLAGS) $< -o $@ ../lib/libauraio_tsan.a -luring -lpthread

test_buffer_tsan: test_buffer.c ../lib/libauraio_tsan.a
	$(CC) $(CFLAGS) $(TSAN_FLAGS) $< -o $@ ../lib/libauraio_tsan.a -luring -lpthread

test_ring_tsan: test_ring.c ../lib/libauraio_tsan.a
	$(CC) $(CFLAGS) $(TSAN_FLAGS) $< -o $@ ../lib/libauraio_tsan.a -luring -lpthread

test_stats_tsan: test_stats.c ../lib/libauraio_tsan.a
	$(CC) $(CFLAGS) $(TSAN_FLAGS) $< -o $@ ../lib/libauraio_tsan.a -luring -lpthread

test_concurrency_stress_tsan: test_concurrency_stress.c ../lib/libauraio_tsan.a
	$(CC) $(CFLAGS) $(TSAN_FLAGS) $< -o $@ ../lib/libauraio_tsan.a -luring -lpthread

# ============================================================================
# AddressSanitizer tests (requires rebuild with ASAN)
# ============================================================================
ASAN_TESTS = test_engine_asan test_buffer_asan test_ring_asan test_stats_asan test_ring_select_asan test_concurrency_stress_asan

asan: $(ASAN_TESTS)
	@echo "Running AddressSanitizer tests..."
	@for t in $(ASAN_TESTS); do \
		echo ""; \
		echo "=== $$t ==="; \
		./$$t || exit 1; \
	done
	@echo ""
	@echo "All ASAN tests passed!"

test_engine_asan: test_engine.c ../lib/libauraio_asan.a
	$(CC) $(CFLAGS) $(ASAN_FLAGS) $< -o $@ ../lib/libauraio_asan.a -luring -lpthread

test_buffer_asan: test_buffer.c ../lib/libauraio_asan.a
	$(CC) $(CFLAGS) $(ASAN_FLAGS) $< -o $@ ../lib/libauraio_asan.a -luring -lpthread

test_ring_asan: test_ring.c ../lib/libauraio_asan.a
	$(CC) $(CFLAGS) $(ASAN_FLAGS) $< -o $@ ../lib/libauraio_asan.a -luring -lpthread

test_stats_asan: test_stats.c ../lib/libauraio_asan.a
	$(CC) $(CFLAGS) $(ASAN_FLAGS) $< -o $@ ../lib/libauraio_asan.a -luring -lpthread

test_concurrency_stress_asan: test_concurrency_stress.c ../lib/libauraio_asan.a
	$(CC) $(CFLAGS) $(ASAN_FLAGS) $< -o $@ ../lib/libauraio_asan.a -luring -lpthread

clean:
	rm -f $(TESTS) $(CPP_TESTS) stress_test perf_bench $(TSAN_TESTS) $(ASAN_TESTS)

# ============================================================================
# Deep Performance Analysis (flamegraphs, cachegrind, pahole, callgrind)
# ============================================================================

deep-analysis: perf_bench
	./run_deep_analysis.sh

deep-analysis-quick: perf_bench
	./run_deep_analysis.sh --quick

deep-analysis-no-valgrind: perf_bench
	./run_deep_analysis.sh --skip-valgrind

.PHONY: all clean valgrind valgrind-quick tsan asan cpp-test stress stress-full \
        ring-select bench bench-throughput bench-latency bench-scalability \
        check-deps perf-test perf-test-quick perf-test-full perf-baseline perf-profile \
        analyze analyze-quick analyze-full analyze-no-fio \
        deep-analysis deep-analysis-quick deep-analysis-no-valgrind
