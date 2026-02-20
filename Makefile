# AuraIO Library Makefile

CC = gcc
CFLAGS = -Wall -Wextra -Wshadow -Wpedantic -Wstrict-prototypes -Wmissing-declarations \
         -std=c11 -O2 -fPIC -fvisibility=hidden -Iengine/include -Iengine/src
HARDEN_CFLAGS = -fstack-protector-strong -U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=3 -Wformat -Wformat-security
HARDEN_LDFLAGS = -Wl,-z,relro,-z,now
LDFLAGS = $(HARDEN_LDFLAGS) -luring -lpthread
CFLAGS += $(HARDEN_CFLAGS)

# Version (keep in sync with engine/include/aura.h)
VERSION_MAJOR = 0
VERSION_MINOR = 5
VERSION_PATCH = 0
VERSION = $(VERSION_MAJOR).$(VERSION_MINOR).$(VERSION_PATCH)

# Source files
SRC = engine/src/aura.c engine/src/adaptive_engine.c engine/src/adaptive_ring.c engine/src/adaptive_buffer.c engine/src/log.c
OBJ = $(SRC:.c=.o)
DEP = $(OBJ:.o=.d)

# Library names (SO versioning: libaura.so -> libaura.so.0 -> libaura.so.0.1.0)
LIB_SHARED = engine/lib/libaura.so.$(VERSION)
LIB_SONAME = libaura.so.$(VERSION_MAJOR)
LIB_LINKNAME = libaura.so
LIB_STATIC = engine/lib/libaura.a

# pkg-config file
PKGCONFIG = engine/lib/libaura.pc

# Installation paths
PREFIX ?= /usr/local
DESTDIR ?=

# Rust environment setup (source cargo if installed via rustup)
CARGO = $(shell command -v cargo 2>/dev/null || echo "$$HOME/.cargo/bin/cargo")
RUST_LIB_PATH = $(CURDIR)/engine/lib

# Integration build settings
INTEGRATION_CFLAGS = $(CFLAGS)
INTEGRATION_LDFLAGS = -Lengine/lib -laura $(LDFLAGS) -Wl,-rpath,'$$ORIGIN/../../../engine/lib'

# =============================================================================
# Core build targets
# =============================================================================

# Default target
# Build library, bindings, and integrations (examples are opt-in via 'make examples')
all: engine rust integrations
.PHONY: all

# Engine library artifacts only (fast path for local iteration)
engine: $(LIB_SHARED) $(LIB_STATIC) $(PKGCONFIG)
.PHONY: engine

# Create lib directory
engine/lib:
	mkdir -p engine/lib

# Generate pkg-config file
$(PKGCONFIG): engine/pkg/libaura.pc.in | engine/lib
	sed -e 's|@PREFIX@|$(PREFIX)|g' \
	    -e 's|@VERSION@|$(VERSION)|g' \
	    $< > $@

# Shared library (with soname for ABI versioning)
$(LIB_SHARED): $(OBJ) | engine/lib
	$(CC) -shared -Wl,-soname,$(LIB_SONAME) -DAURA_SHARED_BUILD -o $@ $^ $(LDFLAGS)
	ln -sf $(notdir $(LIB_SHARED)) engine/lib/$(LIB_SONAME)
	ln -sf $(LIB_SONAME) engine/lib/$(LIB_LINKNAME)

# Static library
$(LIB_STATIC): $(OBJ) | engine/lib
	ar rcs $@ $^

# Object files (AURA_SHARED_BUILD exports public symbols via AURA_API)
# -MMD -MP generates .d dependency files for automatic header tracking
engine/src/%.o: engine/src/%.c
	$(CC) $(CFLAGS) -MMD -MP -DAURA_SHARED_BUILD -c $< -o $@

-include $(DEP)

# Debug build
debug: CFLAGS += -g -O0 -DDEBUG
debug: engine
.PHONY: debug

# =============================================================================
# Integrations (individual + aggregate)
# =============================================================================

integration-prometheus: engine
	$(CC) $(INTEGRATION_CFLAGS) -Iintegrations/prometheus/C \
		integrations/prometheus/C/example.c integrations/prometheus/C/aura_prometheus.c \
		-o integrations/prometheus/C/prometheus_example \
		$(INTEGRATION_LDFLAGS)

integration-otel: engine
	$(CC) $(INTEGRATION_CFLAGS) -Iintegrations/opentelemetry/C \
		integrations/opentelemetry/C/example.c integrations/opentelemetry/C/aura_otel.c integrations/opentelemetry/C/aura_otel_push.c \
		-o integrations/opentelemetry/C/otel_example \
		$(INTEGRATION_LDFLAGS)

integration-syslog: engine
	$(CC) $(INTEGRATION_CFLAGS) -Iintegrations/syslog/C \
		integrations/syslog/C/example.c integrations/syslog/C/aura_syslog.c \
		-o integrations/syslog/C/syslog_example \
		$(INTEGRATION_LDFLAGS)

integrations: integration-prometheus integration-otel integration-syslog
.PHONY: integration-prometheus integration-otel integration-syslog integrations

# =============================================================================
# Rust Bindings
# =============================================================================

# Build Rust bindings
rust: engine
	LD_LIBRARY_PATH=$(RUST_LIB_PATH) $(CARGO) build --manifest-path bindings/rust/Cargo.toml --release

# Run Rust tests
rust-test: engine
	@printf "Running Rust tests...\n"
	@rust_out=$$(LD_LIBRARY_PATH=$(RUST_LIB_PATH) $(CARGO) test --manifest-path bindings/rust/Cargo.toml 2>&1) && rust_ok=1 || rust_ok=0; \
	echo "$$rust_out" | sh build-tools/format-rust-tests.sh; \
	if [ "$$rust_ok" -eq 0 ]; then exit 1; fi

# Build Rust examples
rust-examples: rust
	LD_LIBRARY_PATH=$(RUST_LIB_PATH) $(CARGO) build --manifest-path examples/rust/Cargo.toml --examples --release

# Run a specific Rust example
rust-run-%: rust-examples
	LD_LIBRARY_PATH=$(RUST_LIB_PATH) $(CARGO) run --manifest-path examples/rust/Cargo.toml --example $* --release

# Clean Rust build artifacts
rust-clean:
	-$(CARGO) clean --manifest-path bindings/rust/Cargo.toml 2>/dev/null || true
	-$(CARGO) clean --manifest-path examples/rust/Cargo.toml 2>/dev/null || true

.PHONY: rust rust-test rust-examples rust-clean

# =============================================================================
# C++ and examples
# =============================================================================

# Run C++ tests
cpp-test: engine
	$(MAKE) -C tests cpp-test

# Build all examples (C + C++ + Rust)
examples: c-examples cpp-examples rust-examples

# Build C examples only
c-examples: engine
	$(MAKE) -C examples/C

# Build C++ examples only
cpp-examples: engine
	$(MAKE) -C examples/cpp

.PHONY: cpp-test examples c-examples cpp-examples

# =============================================================================
# Testing
# =============================================================================

# Build and run C tests (local/in-container)
test-local: engine
	$(MAKE) -C tests

# Run all tests (C, C++, Rust) with combined summary
test-all: engine
	@echo "========================================"
	@echo "AuraIO Test Suite"
	@echo "========================================"
	@c_pass=0; c_fail=0; cpp_pass=0; cpp_fail=0; rust_pass=0; rust_fail=0; \
	echo ""; \
	echo "--- C Tests ---"; \
	c_out=$$($(MAKE) -C tests 2>&1) && c_ok=1 || c_ok=0; \
	echo "$$c_out"; \
	c_pass=$$(echo "$$c_out" | grep -A1 'C Tests Summary' | grep -oE '[0-9]+ tests passed' | grep -oE '[0-9]+'); \
	[ -z "$$c_pass" ] && c_pass=0; \
	if [ "$$c_ok" -eq 0 ]; then c_fail=1; fi; \
	echo ""; \
	echo "--- C++ Tests ---"; \
	cpp_out=$$($(MAKE) -C tests cpp-test 2>&1) && cpp_ok=1 || cpp_ok=0; \
	echo "$$cpp_out"; \
	cpp_pass=$$(echo "$$cpp_out" | grep -oE '^[0-9]+ tests passed' | tail -1 | grep -oE '^[0-9]+'); \
	cpp_fail_count=$$(echo "$$cpp_out" | grep -oE '[0-9]+ FAILED' | grep -oE '[0-9]+'); \
	[ -z "$$cpp_pass" ] && cpp_pass=0; \
	[ -z "$$cpp_fail_count" ] && cpp_fail_count=0; \
	if [ "$$cpp_ok" -eq 0 ] || [ "$$cpp_fail_count" -gt 0 ]; then cpp_fail=1; fi; \
	echo ""; \
	echo "--- Rust Tests ---"; \
	rust_out=$$(LD_LIBRARY_PATH=$(RUST_LIB_PATH) $(CARGO) test --manifest-path bindings/rust/Cargo.toml 2>&1) && rust_ok=1 || rust_ok=0; \
	echo "$$rust_out" | sh build-tools/format-rust-tests.sh; \
	rust_pass=$$(echo "$$rust_out" | grep -oE '[0-9]+ passed' | awk '{s+=$$1} END{print s+0}'); \
	rust_ignored=$$(echo "$$rust_out" | grep -oE '[0-9]+ ignored' | awk '{s+=$$1} END{print s+0}'); \
	[ -z "$$rust_pass" ] && rust_pass=0; \
	[ -z "$$rust_ignored" ] && rust_ignored=0; \
	if [ "$$rust_ok" -eq 0 ]; then rust_fail=1; fi; \
	total=$$((c_pass + cpp_pass + rust_pass)); \
	echo ""; \
	echo "========================================"; \
	echo "Test Summary"; \
	echo "========================================"; \
	if [ "$$c_fail" -eq 0 ]; then \
		echo "  C:      $$c_pass passed [OK]"; \
	else \
		echo "  C:      FAILED"; \
	fi; \
	if [ "$$cpp_fail" -eq 0 ]; then \
		echo "  C++:    $$cpp_pass passed [OK]"; \
	else \
		echo "  C++:    FAILED ($$cpp_fail_count failures)"; \
	fi; \
	if [ "$$rust_fail" -eq 0 ]; then \
		if [ "$$rust_ignored" -gt 0 ]; then \
			echo "  Rust:   $$rust_pass passed, $$rust_ignored skipped [OK]"; \
		else \
			echo "  Rust:   $$rust_pass passed [OK]"; \
		fi; \
	else \
		echo "  Rust:   FAILED"; \
	fi; \
	echo "  --------"; \
	echo "  Total:  $$total tests passed"; \
	echo "========================================"; \
	if [ "$$c_fail" -ne 0 ] || [ "$$cpp_fail" -ne 0 ] || [ "$$rust_fail" -ne 0 ]; then \
		echo "RESULT: FAILED"; \
		exit 1; \
	else \
		echo "RESULT: ALL PASSED"; \
	fi

test:
	$(MAKE) -j4 all
	$(MAKE) -j1 test-all

test-memory:
	$(MAKE) -j4 all
	$(MAKE) -j1 test-tsan
	$(MAKE) -j1 test-asan

test-strict:
	$(MAKE) -j4 all integrations
	$(MAKE) -j1 test-all
	$(MAKE) -j1 test-tsan
	$(MAKE) -j1 test-asan

.PHONY: test-local test-all test test-memory test-strict

# =============================================================================
# Sanitizer builds
# =============================================================================

# ThreadSanitizer build
TSAN_CFLAGS = $(CFLAGS) -fsanitize=thread -fPIE -g
TSAN_OBJ = $(SRC:.c=.tsan.o)
LIB_TSAN = engine/lib/libaura_tsan.a

engine/src/%.tsan.o: engine/src/%.c
	$(CC) $(TSAN_CFLAGS) -c $< -o $@

$(LIB_TSAN): $(TSAN_OBJ) | engine/lib
	ar rcs $@ $^

tsan: $(LIB_TSAN)

# AddressSanitizer build
ASAN_CFLAGS = $(CFLAGS) -fsanitize=address -fno-omit-frame-pointer -g
ASAN_OBJ = $(SRC:.c=.asan.o)
LIB_ASAN = engine/lib/libaura_asan.a

engine/src/%.asan.o: engine/src/%.c
	$(CC) $(ASAN_CFLAGS) -c $< -o $@

$(LIB_ASAN): $(ASAN_OBJ) | engine/lib
	ar rcs $@ $^

asan: $(LIB_ASAN)

.PHONY: tsan asan

# =============================================================================
# Sanitizer test targets
# =============================================================================

# Run tests under valgrind
test-valgrind: engine
	$(MAKE) -C tests valgrind

# Run tests with ThreadSanitizer
test-tsan: tsan
	$(MAKE) -C tests tsan

# Run tests with AddressSanitizer
test-asan: asan
	$(MAKE) -C tests asan

# Run all sanitizer tests with summary
test-sanitizers: engine tsan asan
	@echo ""
	@echo "========================================"
	@echo "  Sanitizer Test Suite"
	@echo "========================================"
	@failed=""; \
	for suite in "Valgrind:valgrind" "TSan:tsan" "ASan:asan"; do \
		name=$${suite%%:*}; target=$${suite#*:}; \
		echo ""; \
		echo "--- $$name ---"; \
		if $(MAKE) -C tests $$target 2>&1; then \
			eval $${target}_ok=1; \
		else \
			eval $${target}_ok=0; failed="$$failed $$name"; \
		fi; \
	done; \
	echo ""; \
	echo "========================================"; \
	echo "  Sanitizer Summary"; \
	echo "========================================"; \
	for suite in "Valgrind:valgrind" "TSan:tsan" "ASan:asan"; do \
		name=$${suite%%:*}; target=$${suite#*:}; \
		eval ok=\$$$${target}_ok; \
		if [ "$$ok" -eq 1 ]; then \
			printf "  %-12s[OK]\n" "$$name:"; \
		else \
			printf "  %-12sFAILED\n" "$$name:"; \
		fi; \
	done; \
	echo "========================================"; \
	if [ -n "$$failed" ]; then \
		echo "RESULT: FAILED"; exit 1; \
	else \
		echo "RESULT: ALL PASSED"; \
	fi

.PHONY: test-valgrind test-tsan test-asan test-sanitizers

# =============================================================================
# Performance benchmarks (use BENCH_DIR=/path to override test file location)
# =============================================================================
BENCH_DIR_FLAG = $(if $(BENCH_DIR),--dir $(BENCH_DIR))

# Build perf_bench binary (shared prerequisite for bench targets)
perf_bench: engine
	$(MAKE) -C tests perf_bench
.PHONY: perf_bench

# Quick benchmark (3s per test)
bench-quick: perf_bench
	cd tests && ./run_analysis.sh --quick $(BENCH_DIR_FLAG)

# Standard benchmark with FIO comparison (5s per test)
bench: perf_bench
	cd tests && ./run_analysis.sh --standard $(BENCH_DIR_FLAG)

# Full benchmark (10s per test, more stable numbers)
bench-full: perf_bench
	cd tests && ./run_analysis.sh --full $(BENCH_DIR_FLAG)

# Benchmark without FIO baseline
bench-no-fio: perf_bench
	cd tests && ./run_analysis.sh --skip-fio $(BENCH_DIR_FLAG)

# Check benchmark dependencies (fio, perf, numactl, etc.)
bench-deps: engine
	$(MAKE) -C tests check-deps

# Deep performance analysis (flamegraphs, cachegrind, pahole, callgrind)
bench-deep: perf_bench
	cd tests && ./run_deep_analysis.sh $(BENCH_DIR_FLAG)

# Quick deep analysis (~10 min)
bench-deep-quick: perf_bench
	cd tests && ./run_deep_analysis.sh --quick $(BENCH_DIR_FLAG)

# Performance regression test: raw io_uring vs AuraIO overhead
perf-regression: engine
	$(MAKE) -C tests perf_regression
	cd tests && ./perf_regression $(PERF_REGRESSION_ARGS)

# Adaptive value benchmark: proves AIMD beats static depth tuning
bench-adaptive: engine
	$(MAKE) -C tests adaptive_value
	cd tests && ./adaptive_value $(ADAPTIVE_BENCH_ARGS)

.PHONY: bench bench-quick bench-full bench-no-fio bench-deps bench-deep bench-deep-quick perf-regression bench-adaptive

# =============================================================================
# Tools
# =============================================================================

# Build BFFIO benchmark tool
BFFIO: engine
	$(MAKE) -C tools/BFFIO

# Run BFFIO functional tests
BFFIO-test: BFFIO
	$(MAKE) -C tools/BFFIO test

# Run BFFIO vs FIO baseline comparison
BFFIO-baseline: BFFIO
	$(MAKE) -C tools/BFFIO baseline

# Build auracp file copy tool
auracp: engine
	$(MAKE) -C tools/auracp

# Build C++ auracp file copy tool
auracp-cpp: engine
	$(MAKE) -C tools/auracp_cpp

# Build aura-hash checksum tool
aura-hash: engine
	$(MAKE) -C tools/aura-hash

# Build sspa storage analyzer
sspa: engine
	$(MAKE) -C tools/sspa

# Build atree directory tree tool
atree: engine
	$(MAKE) -C tools/atree

# Build all tools
tools: BFFIO auracp auracp-cpp aura-hash sspa atree

.PHONY: BFFIO BFFIO-test BFFIO-baseline auracp auracp-cpp aura-hash sspa atree tools

# =============================================================================
# Installation
# =============================================================================

install: engine
	install -d $(DESTDIR)$(PREFIX)/lib
	install -d $(DESTDIR)$(PREFIX)/lib/pkgconfig
	install -d $(DESTDIR)$(PREFIX)/include
	install -d $(DESTDIR)$(PREFIX)/include/aura
	install -m 755 $(LIB_SHARED) $(DESTDIR)$(PREFIX)/lib/
	ln -sf $(notdir $(LIB_SHARED)) $(DESTDIR)$(PREFIX)/lib/$(LIB_SONAME)
	ln -sf $(LIB_SONAME) $(DESTDIR)$(PREFIX)/lib/$(LIB_LINKNAME)
	install -m 644 $(LIB_STATIC) $(DESTDIR)$(PREFIX)/lib/
	install -m 644 $(PKGCONFIG) $(DESTDIR)$(PREFIX)/lib/pkgconfig/
	install -m 644 engine/include/aura.h $(DESTDIR)$(PREFIX)/include/
	install -m 644 engine/include/aura/*.hpp $(DESTDIR)$(PREFIX)/include/aura/
	install -d $(DESTDIR)$(PREFIX)/include/aura/integrations
	install -m 644 integrations/prometheus/C/aura_prometheus.h $(DESTDIR)$(PREFIX)/include/aura/integrations/
	install -m 644 integrations/opentelemetry/C/aura_otel.h $(DESTDIR)$(PREFIX)/include/aura/integrations/
	install -m 644 integrations/opentelemetry/C/aura_otel_push.h $(DESTDIR)$(PREFIX)/include/aura/integrations/
	install -m 644 integrations/syslog/C/aura_syslog.h $(DESTDIR)$(PREFIX)/include/aura/integrations/
	ldconfig $(DESTDIR)$(PREFIX)/lib 2>/dev/null || true

# Uninstall
uninstall:
	rm -f $(DESTDIR)$(PREFIX)/lib/libaura.so.$(VERSION)
	rm -f $(DESTDIR)$(PREFIX)/lib/$(LIB_SONAME)
	rm -f $(DESTDIR)$(PREFIX)/lib/$(LIB_LINKNAME)
	rm -f $(DESTDIR)$(PREFIX)/lib/libaura.a
	rm -f $(DESTDIR)$(PREFIX)/lib/pkgconfig/libaura.pc
	rm -f $(DESTDIR)$(PREFIX)/include/aura.h
	rm -rf $(DESTDIR)$(PREFIX)/include/aura

.PHONY: install uninstall

# =============================================================================
# Clean
# =============================================================================

clean: rust-clean
	rm -f $(OBJ) $(DEP) $(TSAN_OBJ) $(ASAN_OBJ)
	rm -f $(LIB_SHARED) engine/lib/$(LIB_SONAME) engine/lib/$(LIB_LINKNAME) $(LIB_STATIC) $(PKGCONFIG) $(LIB_TSAN) $(LIB_ASAN)
	rm -rf engine/lib
	-$(MAKE) -C tests clean 2>/dev/null || true
	-$(MAKE) -C examples/C clean 2>/dev/null || true
	-$(MAKE) -C examples/cpp clean 2>/dev/null || true
	-$(MAKE) -C tools/BFFIO clean 2>/dev/null || true
	-$(MAKE) -C tools/auracp clean 2>/dev/null || true
	-$(MAKE) -C tools/auracp_cpp clean 2>/dev/null || true
	-$(MAKE) -C tools/aura-hash clean 2>/dev/null || true
	-$(MAKE) -C tools/sspa clean 2>/dev/null || true
	-$(MAKE) -C tools/atree clean 2>/dev/null || true
	rm -f integrations/prometheus/C/prometheus_example
	rm -f integrations/opentelemetry/C/otel_example
	rm -f integrations/syslog/C/syslog_example

.PHONY: clean

# =============================================================================
# Dependency management
# =============================================================================

# Install all required dependencies
deps:
	bash build-tools/deps/install_all.sh

# Check dependencies without installing
deps-check:
	bash build-tools/deps/check.sh

.PHONY: deps deps-check

# =============================================================================
# Linting and static analysis
# =============================================================================

# All source files for linting (library + tests + examples)
ALL_SRC = $(SRC) $(wildcard tests/*.c) $(wildcard examples/C/*.c)
HEADERS = $(wildcard engine/include/*.h) $(wildcard engine/src/*.h)

# Run lint: prefer cppcheck, fall back to clang-tidy
lint:
	@if command -v cppcheck >/dev/null 2>&1; then \
		$(MAKE) lint-cppcheck; \
	elif command -v clang-tidy-18 >/dev/null 2>&1; then \
		$(MAKE) lint-clang-tidy; \
	elif command -v clang-tidy >/dev/null 2>&1; then \
		$(MAKE) lint-clang-tidy; \
	else \
		echo "No linter found. Install cppcheck or clang-tidy."; exit 1; \
	fi

# cppcheck - errors and warnings only (CI-safe)
lint-cppcheck:
	@echo "Running cppcheck..."
	@cppcheck --enable=warning,performance,portability \
		--suppress=missingIncludeSystem \
		--suppress=normalCheckLevelMaxBranches \
		--error-exitcode=1 \
		-I engine/include -I engine/src \
		$(SRC) $(HEADERS)

# cppcheck - strict mode with style checks
lint-strict:
	@echo "Running cppcheck (strict)..."
	@cppcheck --check-level=exhaustive \
		--enable=all \
		--suppress=missingIncludeSystem \
		--error-exitcode=1 \
		-I engine/include -I engine/src \
		$(SRC) $(HEADERS)

# clang-tidy (requires compile_commands.json)
lint-clang-tidy: compile_commands.json
	@echo "Running clang-tidy..."
	@clang-tidy $(SRC) -- $(CFLAGS)

.PHONY: lint lint-cppcheck lint-strict lint-clang-tidy

# =============================================================================
# Compile database for IDE/tooling support
# =============================================================================

# Generate compile_commands.json using bear
# Keep this strict: rebuild when source, headers, or build flags change.
compile_commands.json: $(SRC) $(HEADERS) Makefile
	@echo "Generating compile_commands.json..."
	@bear -- $(MAKE) clean all 2>/dev/null || \
		$(MAKE) compdb-manual

# Manual fallback if bear is not installed
compdb-manual:
	@echo "bear not found, generating manually..."
	@echo '[' > compile_commands.json
	@for src in $(SRC); do \
		echo '  {' >> compile_commands.json; \
		echo '    "directory": "$(CURDIR)",' >> compile_commands.json; \
		echo '    "command": "$(CC) $(CFLAGS) -c '$$src'",' >> compile_commands.json; \
		echo '    "file": "'$$src'"' >> compile_commands.json; \
		echo '  },' >> compile_commands.json; \
	done
	@sed -i '$$ s/,$$//' compile_commands.json
	@echo ']' >> compile_commands.json

# Alias for compile database
compdb: compile_commands.json

.PHONY: compdb compdb-manual

# =============================================================================
# Coverage
# =============================================================================

coverage:
	build-tools/coverage/run_llvm_cov.sh coverage

coverage-check:
	MIN_LINE_COVERAGE=$(MIN_LINE_COVERAGE) build-tools/coverage/run_llvm_cov.sh coverage

.PHONY: coverage coverage-check

# =============================================================================
# Help
# =============================================================================

help:
	@echo "AuraIO - Self-tuning async I/O library for Linux"
	@echo ""
	@echo "Build targets:"
	@echo "  make / make all     Build engine library + Rust/C++ bindings + integrations"
	@echo "  make engine         Build libraries and pkg-config file only"
	@echo "  make debug          Build with debug symbols (-g -O0)"
	@echo "  make test           Build library/bindings/integrations + run all tests"
	@echo "  make test-memory    Build library/bindings/integrations + run TSan/ASan tests"
	@echo "  make test-strict    Run test + test-memory"
	@echo "  make test-local     Build and run C unit tests"
	@echo "  make test-all       Run all tests (C, C++, Rust)"
	@echo "  make examples       Build all example programs (C + C++ + Rust)"
	@echo "  make c-examples     Build C example programs only"
	@echo "  make cpp-examples   Build C++ example programs only"
	@echo "  make rust-examples  Build Rust example programs only"
	@echo "  make clean          Remove all build artifacts"
	@echo ""
	@echo "C++ bindings:"
	@echo "  make cpp-test       Build and run C++ unit tests"
	@echo ""
	@echo "Rust bindings:"
	@echo "  make rust           Build Rust bindings"
	@echo "  make rust-test      Run Rust tests"
	@echo "  make rust-clean     Clean Rust build artifacts"
	@echo ""
	@echo "Installation:"
	@echo "  make install        Install to $(PREFIX) (includes pkg-config)"
	@echo "  make uninstall      Remove installed files"
	@echo "  make deps           Install full AuraIO toolchain dependencies"
	@echo "  make deps-check     Verify full AuraIO dependency set"
	@echo ""
	@echo "Sanitizers:"
	@echo "  make tsan           Build library with ThreadSanitizer"
	@echo "  make asan           Build library with AddressSanitizer"
	@echo "  make test-valgrind  Run tests under valgrind"
	@echo "  make test-tsan      Run tests with ThreadSanitizer"
	@echo "  make test-asan      Run tests with AddressSanitizer"
	@echo "  make test-sanitizers Run all sanitizer tests"
	@echo ""
	@echo "Benchmarks:"
	@echo "  make bench          Performance analysis with FIO comparison (5s/test)"
	@echo "  make bench-quick    Quick benchmark (3s/test)"
	@echo "  make bench-full     Full benchmark (10s/test)"
	@echo "  make bench-no-fio   Benchmark without FIO baseline"
	@echo "  make bench-deps     Check benchmark dependencies"
	@echo "  make bench-deep     Deep analysis (flamegraphs, cachegrind, pahole)"
	@echo "  make bench-deep-quick Quick deep analysis (~10 min)"
	@echo "  BENCH_DIR=/path     Override test file directory (default: /tmp/aura_bench)"
	@echo ""
	@echo "Regression testing:"
	@echo "  make perf-regression  Raw io_uring vs AuraIO overhead test"
	@echo "                        Set AURA_PERF_FILE=/path to test against a specific file/device"
	@echo "  make bench-adaptive   Adaptive AIMD vs static depth comparison"
	@echo "                        Set ADAPTIVE_BENCH_ARGS='--quick' for short run"
	@echo ""
	@echo "Code quality:"
	@echo "  make lint           Run cppcheck (errors + warnings)"
	@echo "  make lint-strict    Run cppcheck with style checks"
	@echo "  make lint-clang-tidy Run clang-tidy"
	@echo "  make compdb         Generate compile_commands.json"
	@echo "  make coverage       Build/run tests with llvm-cov and generate coverage artifacts"
	@echo "  make coverage-check MIN_LINE_COVERAGE=N enforce minimum line coverage percentage"
	@echo ""
	@echo "Tools:"
	@echo "  make tools          Build all tools (BFFIO, auracp, aura-hash, sspa, atree)"
	@echo ""
	@echo "BFFIO (Better Faster FIO):"
	@echo "  make BFFIO          Build BFFIO benchmark tool"
	@echo "  make BFFIO-test     Run BFFIO functional tests"
	@echo "  make BFFIO-baseline Run BFFIO vs FIO comparison"
	@echo ""
	@echo "auracp (async file copy):"
	@echo "  make auracp         Build auracp async file copy tool"
	@echo "  make auracp-cpp     Build auracp C++ async file copy tool"
	@echo ""
	@echo "aura-hash (parallel checksum):"
	@echo "  make aura-hash      Build aura-hash parallel checksum tool"
	@echo ""
	@echo "sspa (storage analyzer):"
	@echo "  make sspa           Build sspa storage performance analyzer"
	@echo ""
	@echo "Integrations:"
	@echo "  make integrations   Build integrations (Prometheus + OpenTelemetry + Syslog)"
	@echo ""
	@echo "Variables:"
	@echo "  PREFIX=$(PREFIX)    Installation prefix"
	@echo "  DESTDIR=$(DESTDIR)   Staging directory for packaging"

.PHONY: help
