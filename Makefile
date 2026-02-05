# AuraIO Library Makefile

CC = gcc
CFLAGS = -Wall -Wextra -std=c11 -O2 -fPIC -Iinclude -Isrc
LDFLAGS = -luring -lpthread

# Version (keep in sync with include/auraio.h)
VERSION = 1.0.1

# Source files
SRC = src/auraio.c src/adaptive_engine.c src/adaptive_ring.c src/adaptive_buffer.c
OBJ = $(SRC:.c=.o)

# Library names
LIB_SHARED = lib/libauraio.so
LIB_STATIC = lib/libauraio.a

# pkg-config file
PKGCONFIG = lib/libauraio.pc

# Installation paths
PREFIX ?= /usr/local
DESTDIR ?=

# Default target
all: $(LIB_SHARED) $(LIB_STATIC) $(PKGCONFIG)

# Create lib directory
lib:
	mkdir -p lib

# Generate pkg-config file
$(PKGCONFIG): pkg/libauraio.pc.in | lib
	sed -e 's|@PREFIX@|$(PREFIX)|g' \
	    -e 's|@VERSION@|$(VERSION)|g' \
	    $< > $@

# Shared library
$(LIB_SHARED): $(OBJ) | lib
	$(CC) -shared -o $@ $^ $(LDFLAGS)

# Static library
$(LIB_STATIC): $(OBJ) | lib
	ar rcs $@ $^

# Object files
src/%.o: src/%.c
	$(CC) $(CFLAGS) -c $< -o $@

# Build and run C tests
test: all
	$(MAKE) -C tests

# Run all tests (C, C++, Rust)
test-all: test cpp-test rust-test

# Build examples
examples: all
	$(MAKE) -C examples

# Build C++ examples
cpp-examples: all
	$(MAKE) -C examples cpp-examples

# Run C++ tests
cpp-test: all
	$(MAKE) -C tests cpp-test

# =============================================================================
# Rust Bindings
# =============================================================================

# Rust environment setup (source cargo if installed via rustup)
CARGO = $(shell command -v cargo 2>/dev/null || echo "$$HOME/.cargo/bin/cargo")
RUST_LIB_PATH = $(CURDIR)/lib

# Build Rust bindings
rust: all
	LD_LIBRARY_PATH=$(RUST_LIB_PATH) $(CARGO) build --manifest-path bindings/rust/Cargo.toml --release

# Run Rust tests
rust-test: all
	LD_LIBRARY_PATH=$(RUST_LIB_PATH) $(CARGO) test --manifest-path bindings/rust/Cargo.toml

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
install: all
	install -d $(DESTDIR)$(PREFIX)/lib
	install -d $(DESTDIR)$(PREFIX)/lib/pkgconfig
	install -d $(DESTDIR)$(PREFIX)/include
	install -m 644 $(LIB_SHARED) $(DESTDIR)$(PREFIX)/lib/
	install -m 644 $(LIB_STATIC) $(DESTDIR)$(PREFIX)/lib/
	install -m 644 $(PKGCONFIG) $(DESTDIR)$(PREFIX)/lib/pkgconfig/
	install -m 644 include/auraio.h $(DESTDIR)$(PREFIX)/include/
	ldconfig $(DESTDIR)$(PREFIX)/lib 2>/dev/null || true

# Uninstall
uninstall:
	rm -f $(DESTDIR)$(PREFIX)/lib/libauraio.so
	rm -f $(DESTDIR)$(PREFIX)/lib/libauraio.a
	rm -f $(DESTDIR)$(PREFIX)/lib/pkgconfig/libauraio.pc
	rm -f $(DESTDIR)$(PREFIX)/include/auraio.h

# Clean
clean: rust-clean
	rm -f $(OBJ) $(TSAN_OBJ) $(ASAN_OBJ)
	rm -f $(LIB_SHARED) $(LIB_STATIC) $(PKGCONFIG) $(LIB_TSAN) $(LIB_ASAN)
	rm -rf lib
	-$(MAKE) -C tests clean 2>/dev/null || true
	-$(MAKE) -C examples clean 2>/dev/null || true

# Debug build
debug: CFLAGS += -g -O0 -DDEBUG
debug: all

# =============================================================================
# Sanitizer builds
# =============================================================================

# ThreadSanitizer build
TSAN_CFLAGS = $(CFLAGS) -fsanitize=thread -fPIE -g
TSAN_OBJ = $(SRC:.c=.tsan.o)
LIB_TSAN = lib/libauraio_tsan.a

src/%.tsan.o: src/%.c
	$(CC) $(TSAN_CFLAGS) -c $< -o $@

$(LIB_TSAN): $(TSAN_OBJ) | lib
	ar rcs $@ $^

tsan: $(LIB_TSAN)

# AddressSanitizer build
ASAN_CFLAGS = $(CFLAGS) -fsanitize=address -fno-omit-frame-pointer -g
ASAN_OBJ = $(SRC:.c=.asan.o)
LIB_ASAN = lib/libauraio_asan.a

src/%.asan.o: src/%.c
	$(CC) $(ASAN_CFLAGS) -c $< -o $@

$(LIB_ASAN): $(ASAN_OBJ) | lib
	ar rcs $@ $^

asan: $(LIB_ASAN)

# =============================================================================
# Sanitizer test targets
# =============================================================================

# Run tests under valgrind
test-valgrind: all
	$(MAKE) -C tests valgrind

# Run tests with ThreadSanitizer
test-tsan: tsan
	$(MAKE) -C tests tsan

# Run tests with AddressSanitizer
test-asan: asan
	$(MAKE) -C tests asan

# Run all sanitizer tests
test-sanitizers: test-valgrind test-tsan test-asan

# Dependency checking
deps-check:
	@echo "Checking dependencies..."
	@pkg-config --exists liburing && echo "  liburing: OK" || echo "  liburing: MISSING (apt install liburing-dev)"
	@echo "  pthread: OK (always available)"

# =============================================================================
# Linting and static analysis
# =============================================================================

# All source files for linting (library + tests + examples)
ALL_SRC = $(SRC) $(wildcard tests/*.c) $(wildcard examples/*.c)
HEADERS = $(wildcard include/*.h) $(wildcard src/*.h)

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
		--error-exitcode=1 \
		-I include -I src \
		$(SRC) $(HEADERS)

# cppcheck - strict mode with style checks
lint-strict:
	@echo "Running cppcheck (strict)..."
	@cppcheck --enable=all \
		--suppress=missingIncludeSystem \
		--error-exitcode=1 \
		-I include -I src \
		$(SRC) $(HEADERS)

# clang-tidy (requires compile_commands.json)
lint-clang-tidy: compile_commands.json
	@echo "Running clang-tidy..."
	@clang-tidy $(SRC) -- $(CFLAGS)

# =============================================================================
# Compile database for IDE/tooling support
# =============================================================================

# Generate compile_commands.json using bear
compile_commands.json: $(SRC)
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

# =============================================================================
# Help
# =============================================================================

help:
	@echo "AuraIO - Self-tuning async I/O library for Linux"
	@echo ""
	@echo "Build targets:"
	@echo "  make / make all     Build libraries and pkg-config file"
	@echo "  make debug          Build with debug symbols (-g -O0)"
	@echo "  make test           Build and run C unit tests"
	@echo "  make test-all       Run all tests (C, C++, Rust)"
	@echo "  make examples       Build C example programs"
	@echo "  make clean          Remove all build artifacts"
	@echo ""
	@echo "C++ bindings:"
	@echo "  make cpp-test       Build and run C++ unit tests"
	@echo "  make cpp-examples   Build C++ example programs"
	@echo ""
	@echo "Rust bindings:"
	@echo "  make rust           Build Rust bindings"
	@echo "  make rust-test      Run Rust tests"
	@echo "  make rust-examples  Build Rust example programs"
	@echo "  make rust-clean     Clean Rust build artifacts"
	@echo ""
	@echo "Installation:"
	@echo "  make install        Install to $(PREFIX) (includes pkg-config)"
	@echo "  make uninstall      Remove installed files"
	@echo "  make deps-check     Verify required dependencies"
	@echo ""
	@echo "Sanitizers:"
	@echo "  make tsan           Build library with ThreadSanitizer"
	@echo "  make asan           Build library with AddressSanitizer"
	@echo "  make test-valgrind  Run tests under valgrind"
	@echo "  make test-tsan      Run tests with ThreadSanitizer"
	@echo "  make test-asan      Run tests with AddressSanitizer"
	@echo "  make test-sanitizers Run all sanitizer tests"
	@echo ""
	@echo "Code quality:"
	@echo "  make lint           Run cppcheck (errors + warnings)"
	@echo "  make lint-strict    Run cppcheck with style checks"
	@echo "  make lint-clang-tidy Run clang-tidy"
	@echo "  make compdb         Generate compile_commands.json"
	@echo ""
	@echo "Variables:"
	@echo "  PREFIX=$(PREFIX)    Installation prefix"
	@echo "  DESTDIR=$(DESTDIR)   Staging directory for packaging"

.PHONY: all test test-all examples install uninstall clean debug deps-check help \
        cpp-test cpp-examples \
        rust rust-test rust-examples rust-clean \
        tsan asan test-valgrind test-tsan test-asan test-sanitizers \
        lint lint-cppcheck lint-strict lint-clang-tidy compdb compdb-manual
