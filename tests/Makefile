# AuraIO Tests Makefile

CC = gcc
CXX = g++
CFLAGS = -Wall -Wextra -std=c11 -g -I../include -I../src
CXXFLAGS = -Wall -Wextra -std=c++20 -g -I../include -fcoroutines
LDFLAGS = -L../lib -lauraio -luring -lpthread
RPATH = -Wl,-rpath,'$$ORIGIN/../lib'

# Sanitizer flags
TSAN_FLAGS = -fsanitize=thread -fPIE -pie
ASAN_FLAGS = -fsanitize=address -fno-omit-frame-pointer

TESTS = test_engine test_buffer test_ring
CPP_TESTS = test_cpp

all: $(TESTS)
	@echo "Running all tests..."
	@for t in $(TESTS); do \
		echo ""; \
		echo "=== $$t ==="; \
		./$$t || exit 1; \
	done
	@echo ""
	@echo "All tests passed!"

test_engine: test_engine.c ../lib/libauraio.a
	$(CC) $(CFLAGS) $< -o $@ ../lib/libauraio.a $(LDFLAGS) $(RPATH)

test_buffer: test_buffer.c ../lib/libauraio.a
	$(CC) $(CFLAGS) $< -o $@ ../lib/libauraio.a $(LDFLAGS) $(RPATH)

test_ring: test_ring.c ../lib/libauraio.a
	$(CC) $(CFLAGS) $< -o $@ ../lib/libauraio.a $(LDFLAGS) $(RPATH)

# ============================================================================
# C++ Tests
# ============================================================================

test_cpp: test_cpp.cpp ../lib/libauraio.so
	$(CXX) $(CXXFLAGS) $< -o $@ $(LDFLAGS) $(RPATH)

test-cpp: test_cpp
	./test_cpp

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
		valgrind --leak-check=summary --error-exitcode=1 ./$$t 2>&1 | \
			grep -E "(ERROR SUMMARY|definitely lost|indirectly lost)" || exit 1; \
	done
	@echo "All quick valgrind tests passed!"

# ============================================================================
# ThreadSanitizer tests (requires rebuild with TSAN)
# ============================================================================
TSAN_TESTS = test_engine_tsan test_buffer_tsan test_ring_tsan

tsan: $(TSAN_TESTS)
	@echo "Running ThreadSanitizer tests..."
	@for t in $(TSAN_TESTS); do \
		echo ""; \
		echo "=== $$t ==="; \
		./$$t || exit 1; \
	done
	@echo ""
	@echo "All TSAN tests passed!"

test_engine_tsan: test_engine.c ../lib/libauraio_tsan.a
	$(CC) $(CFLAGS) $(TSAN_FLAGS) $< -o $@ ../lib/libauraio_tsan.a -luring -lpthread

test_buffer_tsan: test_buffer.c ../lib/libauraio_tsan.a
	$(CC) $(CFLAGS) $(TSAN_FLAGS) $< -o $@ ../lib/libauraio_tsan.a -luring -lpthread

test_ring_tsan: test_ring.c ../lib/libauraio_tsan.a
	$(CC) $(CFLAGS) $(TSAN_FLAGS) $< -o $@ ../lib/libauraio_tsan.a -luring -lpthread

# ============================================================================
# AddressSanitizer tests (requires rebuild with ASAN)
# ============================================================================
ASAN_TESTS = test_engine_asan test_buffer_asan test_ring_asan

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

clean:
	rm -f $(TESTS) $(CPP_TESTS) stress_test $(TSAN_TESTS) $(ASAN_TESTS)

.PHONY: all clean valgrind valgrind-quick tsan asan test-cpp stress stress-full
