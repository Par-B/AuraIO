# AuraIO Library Makefile

CC = gcc
CFLAGS = -Wall -Wextra -Wshadow -Wpedantic -Wstrict-prototypes -Wmissing-declarations \
         -std=c11 -O2 -fPIC -fvisibility=hidden -Icore/include -Icore/src
HARDEN_CFLAGS = -fstack-protector-strong -U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=3 -Wformat -Wformat-security
HARDEN_LDFLAGS = -Wl,-z,relro,-z,now
LDFLAGS = $(HARDEN_LDFLAGS) -luring -lpthread
CFLAGS += $(HARDEN_CFLAGS)

# Version (keep in sync with core/include/auraio.h)
VERSION_MAJOR = 0
VERSION_MINOR = 2
VERSION_PATCH = 0
VERSION = $(VERSION_MAJOR).$(VERSION_MINOR).$(VERSION_PATCH)

# Source files
SRC = core/src/auraio.c core/src/adaptive_engine.c core/src/adaptive_ring.c core/src/adaptive_buffer.c core/src/log.c
OBJ = $(SRC:.c=.o)
DEP = $(OBJ:.o=.d)

# Library names (SO versioning: libauraio.so -> libauraio.so.0 -> libauraio.so.0.1.0)
LIB_SHARED = core/lib/libauraio.so.$(VERSION)
LIB_SONAME = libauraio.so.$(VERSION_MAJOR)
LIB_LINKNAME = libauraio.so
LIB_STATIC = core/lib/libauraio.a

# pkg-config file
PKGCONFIG = core/lib/libauraio.pc

# Installation paths
PREFIX ?= /usr/local
DESTDIR ?=

# Default target
# Build library, bindings, and integrations (examples are opt-in via 'make examples')
all: core rust integrations

# Core library artifacts only (fast path for local iteration)
core: $(LIB_SHARED) $(LIB_STATIC) $(PKGCONFIG)

# Create lib directory
core/lib:
	mkdir -p core/lib

# Generate pkg-config file
$(PKGCONFIG): core/pkg/libauraio.pc.in | core/lib
	sed -e 's|@PREFIX@|$(PREFIX)|g' \
	    -e 's|@VERSION@|$(VERSION)|g' \
	    $< > $@

# Shared library (with soname for ABI versioning)
$(LIB_SHARED): $(OBJ) | core/lib
	$(CC) -shared -Wl,-soname,$(LIB_SONAME) -DAURAIO_SHARED_BUILD -o $@ $^ $(LDFLAGS)
	ln -sf $(notdir $(LIB_SHARED)) core/lib/$(LIB_SONAME)
	ln -sf $(LIB_SONAME) core/lib/$(LIB_LINKNAME)

# Static library
$(LIB_STATIC): $(OBJ) | core/lib
	ar rcs $@ $^

# Object files (AURAIO_SHARED_BUILD exports public symbols via AURAIO_API)
# -MMD -MP generates .d dependency files for automatic header tracking
core/src/%.o: core/src/%.c
	$(CC) $(CFLAGS) -MMD -MP -DAURAIO_SHARED_BUILD -c $< -o $@

-include $(DEP)

# Build and run C tests (local/in-container)
test-local: core
	$(MAKE) -C tests

# Run all tests (C, C++, Rust) with combined summary
test-all: core
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
	echo "$$rust_out" | awk ' \
		/^test / { \
			s=$$NF; line=$$0; \
			sub(/^test /, "", line); sub(/ \.\.\. [a-zA-Z]+$$/, "", line); \
			sub(/^tests::/, "", line); \
			if (line ~ / - /) { \
				path=line; sub(/ - .*/, "", path); \
				sub(/^.+\.rs - /, "", line); \
				gsub(/\(line [0-9]+\)/, "", line); \
				sub(/ *- *compile */, "", line); \
				gsub(/^ +| +$$/, "", line); \
				if (line == "") { n=split(path,a,"/"); line=a[n]; sub(/\.rs$$/, "", line) } \
			} \
			if (s=="ok") printf "  %-40s OK\n", line; \
			else if (s=="FAILED") printf "  %-40s FAIL\n", line; \
			else if (s=="ignored") printf "  %-40s SKIP\n", line; \
		}'; \
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

# Build metrics integrations (Prometheus + OpenTelemetry)
integrations: core
	$(CC) $(CFLAGS) -Iintegrations/prometheus/C \
		integrations/prometheus/C/example.c integrations/prometheus/C/auraio_prometheus.c \
		-o integrations/prometheus/C/prometheus_example \
		-Lcore/lib -lauraio $(LDFLAGS) -Wl,-rpath,'$$ORIGIN/../../../core/lib'
	$(CC) $(CFLAGS) -Iintegrations/opentelemetry/C \
		integrations/opentelemetry/C/example.c integrations/opentelemetry/C/auraio_otel.c integrations/opentelemetry/C/auraio_otel_push.c \
		-o integrations/opentelemetry/C/otel_example \
		-Lcore/lib -lauraio $(LDFLAGS) -Wl,-rpath,'$$ORIGIN/../../../core/lib'
	$(CC) $(CFLAGS) -Iintegrations/syslog/C \
		integrations/syslog/C/example.c integrations/syslog/C/auraio_syslog.c \
		-o integrations/syslog/C/syslog_example \
		-Lcore/lib -lauraio $(LDFLAGS) -Wl,-rpath,'$$ORIGIN/../../../core/lib'

# Build BFFIO benchmark tool
BFFIO: core
	$(MAKE) -C tools/BFFIO

# Run BFFIO functional tests
BFFIO-test: BFFIO
	$(MAKE) -C tools/BFFIO test

# Run BFFIO vs FIO baseline comparison
BFFIO-baseline: BFFIO
	$(MAKE) -C tools/BFFIO baseline

# Build all examples (C + C++ + Rust)
examples: c-examples cpp-examples rust-examples

# Build C examples only
c-examples: core
	$(MAKE) -C examples/C

# Build C++ examples only
cpp-examples: core
	$(MAKE) -C examples/cpp

# Run C++ tests
cpp-test: core
	$(MAKE) -C tests cpp-test

# =============================================================================
# Rust Bindings
# =============================================================================

# Rust environment setup (source cargo if installed via rustup)
CARGO = $(shell command -v cargo 2>/dev/null || echo "$$HOME/.cargo/bin/cargo")
RUST_LIB_PATH = $(CURDIR)/core/lib

# Build Rust bindings
rust: core
	LD_LIBRARY_PATH=$(RUST_LIB_PATH) $(CARGO) build --manifest-path bindings/rust/Cargo.toml --release

# Run Rust tests
rust-test: core
	@printf "Running Rust tests...\n"
	@rust_out=$$(LD_LIBRARY_PATH=$(RUST_LIB_PATH) $(CARGO) test --manifest-path bindings/rust/Cargo.toml 2>&1) && rust_ok=1 || rust_ok=0; \
	echo "$$rust_out" | awk ' \
		/^test / { \
			s=$$NF; line=$$0; \
			sub(/^test /, "", line); sub(/ \.\.\. [a-zA-Z]+$$/, "", line); \
			sub(/^tests::/, "", line); \
			if (line ~ / - /) { \
				path=line; sub(/ - .*/, "", path); \
				sub(/^.+\.rs - /, "", line); \
				gsub(/\(line [0-9]+\)/, "", line); \
				sub(/ *- *compile */, "", line); \
				gsub(/^ +| +$$/, "", line); \
				if (line == "") { n=split(path,a,"/"); line=a[n]; sub(/\.rs$$/, "", line) } \
			} \
			if (s=="ok") { printf "  %-40s OK\n", line; p++ } \
			else if (s=="FAILED") { printf "  %-40s FAIL\n", line; f++ } \
			else if (s=="ignored") { printf "  %-40s SKIP\n", line; ig++ } \
		} \
		END { \
			if (f>0) printf "\n%d tests passed, %d FAILED\n",p,f; \
			else if (ig>0) printf "\n%d tests passed, %d skipped\n",p,ig; \
			else printf "\n%d tests passed\n",p \
		}'; \
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

# Install
install: core
	install -d $(DESTDIR)$(PREFIX)/lib
	install -d $(DESTDIR)$(PREFIX)/lib/pkgconfig
	install -d $(DESTDIR)$(PREFIX)/include
	install -d $(DESTDIR)$(PREFIX)/include/auraio
	install -m 755 $(LIB_SHARED) $(DESTDIR)$(PREFIX)/lib/
	ln -sf $(notdir $(LIB_SHARED)) $(DESTDIR)$(PREFIX)/lib/$(LIB_SONAME)
	ln -sf $(LIB_SONAME) $(DESTDIR)$(PREFIX)/lib/$(LIB_LINKNAME)
	install -m 644 $(LIB_STATIC) $(DESTDIR)$(PREFIX)/lib/
	install -m 644 $(PKGCONFIG) $(DESTDIR)$(PREFIX)/lib/pkgconfig/
	install -m 644 core/include/auraio.h $(DESTDIR)$(PREFIX)/include/
	install -m 644 core/include/auraio/*.hpp $(DESTDIR)$(PREFIX)/include/auraio/
	install -d $(DESTDIR)$(PREFIX)/include/auraio/integrations
	install -m 644 integrations/prometheus/C/auraio_prometheus.h $(DESTDIR)$(PREFIX)/include/auraio/integrations/
	install -m 644 integrations/opentelemetry/C/auraio_otel.h $(DESTDIR)$(PREFIX)/include/auraio/integrations/
	install -m 644 integrations/opentelemetry/C/auraio_otel_push.h $(DESTDIR)$(PREFIX)/include/auraio/integrations/
	install -m 644 integrations/syslog/C/auraio_syslog.h $(DESTDIR)$(PREFIX)/include/auraio/integrations/
	ldconfig $(DESTDIR)$(PREFIX)/lib 2>/dev/null || true

# Uninstall
uninstall:
	rm -f $(DESTDIR)$(PREFIX)/lib/libauraio.so.$(VERSION)
	rm -f $(DESTDIR)$(PREFIX)/lib/$(LIB_SONAME)
	rm -f $(DESTDIR)$(PREFIX)/lib/$(LIB_LINKNAME)
	rm -f $(DESTDIR)$(PREFIX)/lib/libauraio.a
	rm -f $(DESTDIR)$(PREFIX)/lib/pkgconfig/libauraio.pc
	rm -f $(DESTDIR)$(PREFIX)/include/auraio.h
	rm -rf $(DESTDIR)$(PREFIX)/include/auraio

# Clean
clean: rust-clean
	rm -f $(OBJ) $(DEP) $(TSAN_OBJ) $(ASAN_OBJ)
	rm -f $(LIB_SHARED) core/lib/$(LIB_SONAME) core/lib/$(LIB_LINKNAME) $(LIB_STATIC) $(PKGCONFIG) $(LIB_TSAN) $(LIB_ASAN)
	rm -rf core/lib
	-$(MAKE) -C tests clean 2>/dev/null || true
	-$(MAKE) -C examples/C clean 2>/dev/null || true
	-$(MAKE) -C examples/cpp clean 2>/dev/null || true
	-$(MAKE) -C tools/BFFIO clean 2>/dev/null || true
	rm -f integrations/prometheus/C/prometheus_example
	rm -f integrations/opentelemetry/C/otel_example
	rm -f integrations/syslog/C/syslog_example

# Debug build
debug: CFLAGS += -g -O0 -DDEBUG
debug: core

# =============================================================================
# Sanitizer builds
# =============================================================================

# ThreadSanitizer build
TSAN_CFLAGS = $(CFLAGS) -fsanitize=thread -fPIE -g
TSAN_OBJ = $(SRC:.c=.tsan.o)
LIB_TSAN = core/lib/libauraio_tsan.a

core/src/%.tsan.o: core/src/%.c
	$(CC) $(TSAN_CFLAGS) -c $< -o $@

$(LIB_TSAN): $(TSAN_OBJ) | core/lib
	ar rcs $@ $^

tsan: $(LIB_TSAN)

# AddressSanitizer build
ASAN_CFLAGS = $(CFLAGS) -fsanitize=address -fno-omit-frame-pointer -g
ASAN_OBJ = $(SRC:.c=.asan.o)
LIB_ASAN = core/lib/libauraio_asan.a

core/src/%.asan.o: core/src/%.c
	$(CC) $(ASAN_CFLAGS) -c $< -o $@

$(LIB_ASAN): $(ASAN_OBJ) | core/lib
	ar rcs $@ $^

asan: $(LIB_ASAN)

# =============================================================================
# Sanitizer test targets
# =============================================================================

# Run tests under valgrind
test-valgrind: core
	$(MAKE) -C tests valgrind

# Run tests with ThreadSanitizer
test-tsan: tsan
	$(MAKE) -C tests tsan

# Run tests with AddressSanitizer
test-asan: asan
	$(MAKE) -C tests asan

# Run all sanitizer tests with summary
test-sanitizers: core tsan asan
	@echo ""
	@echo "========================================"
	@echo "  Sanitizer Test Suite"
	@echo "========================================"
	@vg_fail=0; tsan_fail=0; asan_fail=0; \
	echo ""; \
	echo "--- Valgrind ---"; \
	$(MAKE) -C tests valgrind 2>&1 && vg_ok=1 || vg_ok=0; \
	if [ "$$vg_ok" -eq 0 ]; then vg_fail=1; fi; \
	echo ""; \
	echo "--- ThreadSanitizer ---"; \
	$(MAKE) -C tests tsan 2>&1 && tsan_ok=1 || tsan_ok=0; \
	if [ "$$tsan_ok" -eq 0 ]; then tsan_fail=1; fi; \
	echo ""; \
	echo "--- AddressSanitizer ---"; \
	$(MAKE) -C tests asan 2>&1 && asan_ok=1 || asan_ok=0; \
	if [ "$$asan_ok" -eq 0 ]; then asan_fail=1; fi; \
	echo ""; \
	echo "========================================"; \
	echo "  Sanitizer Summary"; \
	echo "========================================"; \
	if [ "$$vg_fail" -eq 0 ]; then \
		echo "  Valgrind:  [OK]"; \
	else \
		echo "  Valgrind:  FAILED"; \
	fi; \
	if [ "$$tsan_fail" -eq 0 ]; then \
		echo "  TSan:      [OK]"; \
	else \
		echo "  TSan:      FAILED"; \
	fi; \
	if [ "$$asan_fail" -eq 0 ]; then \
		echo "  ASan:      [OK]"; \
	else \
		echo "  ASan:      FAILED"; \
	fi; \
	echo "========================================"; \
	if [ "$$vg_fail" -ne 0 ] || [ "$$tsan_fail" -ne 0 ] || [ "$$asan_fail" -ne 0 ]; then \
		echo "RESULT: FAILED"; exit 1; \
	else \
		echo "RESULT: ALL PASSED"; \
	fi

# =============================================================================
# Performance benchmarks (use BENCH_DIR=/path to override test file location)
# =============================================================================
BENCH_DIR_FLAG = $(if $(BENCH_DIR),--dir $(BENCH_DIR))

# Quick benchmark (3s per test)
bench-quick: core
	$(MAKE) -C tests perf_bench
	cd tests && ./run_analysis.sh --quick $(BENCH_DIR_FLAG)

# Standard benchmark with FIO comparison (5s per test)
bench: core
	$(MAKE) -C tests perf_bench
	cd tests && ./run_analysis.sh --standard $(BENCH_DIR_FLAG)

# Full benchmark (10s per test, more stable numbers)
bench-full: core
	$(MAKE) -C tests perf_bench
	cd tests && ./run_analysis.sh --full $(BENCH_DIR_FLAG)

# Benchmark without FIO baseline
bench-no-fio: core
	$(MAKE) -C tests perf_bench
	cd tests && ./run_analysis.sh --skip-fio $(BENCH_DIR_FLAG)

# Check benchmark dependencies (fio, perf, numactl, etc.)
bench-deps: core
	$(MAKE) -C tests check-deps

# Deep performance analysis (flamegraphs, cachegrind, pahole, callgrind)
bench-deep: core
	$(MAKE) -C tests perf_bench
	cd tests && ./run_deep_analysis.sh $(BENCH_DIR_FLAG)

# Quick deep analysis (~10 min)
bench-deep-quick: core
	$(MAKE) -C tests perf_bench
	cd tests && ./run_deep_analysis.sh --quick $(BENCH_DIR_FLAG)

# =============================================================================
# Dependency management
# =============================================================================

# Install all required dependencies
deps:
	bash tools/deps/install_all.sh

# Check dependencies without installing
deps-check:
	bash tools/deps/check.sh

# =============================================================================
# Linting and static analysis
# =============================================================================

# All source files for linting (library + tests + examples)
ALL_SRC = $(SRC) $(wildcard tests/*.c) $(wildcard examples/*.c)
HEADERS = $(wildcard core/include/*.h) $(wildcard core/src/*.h)

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
		-I core/include -I core/src \
		$(SRC) $(HEADERS)

# cppcheck - strict mode with style checks
lint-strict:
	@echo "Running cppcheck (strict)..."
	@cppcheck --check-level=exhaustive \
		--enable=all \
		--suppress=missingIncludeSystem \
		--error-exitcode=1 \
		-I core/include -I core/src \
		$(SRC) $(HEADERS)

# clang-tidy (requires compile_commands.json)
lint-clang-tidy: compile_commands.json
	@echo "Running clang-tidy..."
	@clang-tidy $(SRC) -- $(CFLAGS)

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

coverage:
	tools/coverage/run_llvm_cov.sh coverage

coverage-check:
	MIN_LINE_COVERAGE=$(MIN_LINE_COVERAGE) tools/coverage/run_llvm_cov.sh coverage

# =============================================================================
# Help
# =============================================================================

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

help:
	@echo "AuraIO - Self-tuning async I/O library for Linux"
	@echo ""
	@echo "Build targets:"
	@echo "  make / make all     Build core library + Rust/C++ bindings + integrations"
	@echo "  make core           Build libraries and pkg-config file only"
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
	@echo "  BENCH_DIR=/path     Override test file directory (default: /tmp/auraio_bench)"
	@echo ""
	@echo "Code quality:"
	@echo "  make lint           Run cppcheck (errors + warnings)"
	@echo "  make lint-strict    Run cppcheck with style checks"
	@echo "  make lint-clang-tidy Run clang-tidy"
	@echo "  make compdb         Generate compile_commands.json"
	@echo "  make coverage       Build/run tests with llvm-cov and generate coverage artifacts"
	@echo "  make coverage-check MIN_LINE_COVERAGE=N enforce minimum line coverage percentage"
	@echo ""
	@echo "BFFIO (Better Faster FIO):"
	@echo "  make BFFIO          Build BFFIO benchmark tool"
	@echo "  make BFFIO-test     Run BFFIO functional tests"
	@echo "  make BFFIO-baseline Run BFFIO vs FIO comparison"
	@echo ""
	@echo "Integrations:"
	@echo "  make integrations   Build integrations (Prometheus + OpenTelemetry + Syslog)"
	@echo ""
	@echo "Variables:"
	@echo "  PREFIX=$(PREFIX)    Installation prefix"
	@echo "  DESTDIR=$(DESTDIR)   Staging directory for packaging"

.PHONY: all core test test-memory test-strict test-local test-all examples c-examples cpp-examples install uninstall clean debug deps deps-check help \
        cpp-test \
        rust rust-test rust-examples rust-clean \
	        tsan asan test-valgrind test-tsan test-asan test-sanitizers \
	        bench bench-quick bench-full bench-no-fio bench-deps bench-deep bench-deep-quick \
	        lint lint-cppcheck lint-strict lint-clang-tidy compdb compdb-manual \
	        coverage coverage-check \
	        integrations \
	        BFFIO BFFIO-test BFFIO-baseline
