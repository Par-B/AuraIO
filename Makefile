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

# Run all tests (C, C++, Rust) with combined summary
test-all: all
	@echo "========================================"
	@echo "AuraIO Test Suite"
	@echo "========================================"
	@c_pass=0; c_fail=0; cpp_pass=0; cpp_fail=0; rust_pass=0; rust_fail=0; \
	echo ""; \
	echo "--- C Tests ---"; \
	c_out=$$($(MAKE) -C tests 2>&1) && c_ok=1 || c_ok=0; \
	echo "$$c_out"; \
	c_pass=$$(echo "$$c_out" | grep -oE '^[0-9]+ tests passed' | tail -1 | grep -oE '^[0-9]+'); \
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

# Run all sanitizer tests with summary
test-sanitizers: all tsan asan
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
# Dependency management
# =============================================================================

# Install all required dependencies
deps:
	@echo "========================================"
	@echo "AuraIO Dependency Installer"
	@echo "========================================"
	@echo ""
	@# --- Kernel version check ---
	@echo "--- Checking kernel version ---"
	@KMAJOR=$$(uname -r | cut -d. -f1); \
	if [ "$$KMAJOR" -lt 6 ] 2>/dev/null; then \
		echo "  [FAIL] Kernel $$(uname -r) — AuraIO requires kernel >= 6.0"; \
		echo "         io_uring features (registered buffers, SQPOLL) need v6+"; \
		exit 1; \
	else \
		echo "  [OK] Kernel $$(uname -r)"; \
	fi
	@echo ""
	@# --- Check sudo availability ---
	@echo "--- Checking sudo access ---"
	@if command -v sudo >/dev/null 2>&1 && sudo -n true 2>/dev/null; then \
		echo "  [OK] sudo available"; \
	elif command -v sudo >/dev/null 2>&1; then \
		echo "  [OK] sudo available (may prompt for password)"; \
	else \
		echo "  [WARN] sudo not available — cannot install packages"; \
		echo "         Install manually: apt-get install gcc g++ make liburing-dev pkg-config python3"; \
	fi
	@echo ""
	@# --- Mandatory packages (C, C++, liburing, python3) ---
	@echo "--- Installing mandatory packages ---"
	@if command -v sudo >/dev/null 2>&1; then \
		echo "  Installing: gcc g++ make liburing-dev pkg-config python3..."; \
		sudo apt-get install -y gcc g++ make liburing-dev pkg-config python3 \
			&& echo "  [OK] Mandatory packages installed" \
			|| { echo "  [FAIL] apt-get install failed"; exit 1; }; \
	else \
		echo "  [SKIP] No sudo — checking if packages are already present..."; \
		MISSING=""; \
		command -v gcc >/dev/null 2>&1 || MISSING="$$MISSING gcc"; \
		command -v g++ >/dev/null 2>&1 || MISSING="$$MISSING g++"; \
		command -v make >/dev/null 2>&1 || MISSING="$$MISSING make"; \
		command -v pkg-config >/dev/null 2>&1 || MISSING="$$MISSING pkg-config"; \
		command -v python3 >/dev/null 2>&1 || MISSING="$$MISSING python3"; \
		pkg-config --exists liburing 2>/dev/null || MISSING="$$MISSING liburing-dev"; \
		if [ -n "$$MISSING" ]; then \
			echo "  [FAIL] Missing packages:$$MISSING"; \
			echo "         Run: sudo apt-get install -y$$MISSING"; \
			exit 1; \
		else \
			echo "  [OK] All mandatory packages already present"; \
		fi; \
	fi
	@echo ""
	@# --- Verify mandatory packages ---
	@echo "--- Verifying mandatory packages ---"
	@FAIL=0; \
	command -v gcc >/dev/null 2>&1 \
		&& echo "  [OK] gcc: $$(gcc --version | head -1)" \
		|| { echo "  [FAIL] gcc not found"; FAIL=1; }; \
	command -v g++ >/dev/null 2>&1 \
		&& echo "  [OK] g++: $$(g++ --version | head -1)" \
		|| { echo "  [FAIL] g++ not found"; FAIL=1; }; \
	command -v make >/dev/null 2>&1 \
		&& echo "  [OK] make: $$(make --version | head -1)" \
		|| { echo "  [FAIL] make not found"; FAIL=1; }; \
	command -v pkg-config >/dev/null 2>&1 \
		&& echo "  [OK] pkg-config: $$(pkg-config --version)" \
		|| { echo "  [FAIL] pkg-config not found"; FAIL=1; }; \
	command -v python3 >/dev/null 2>&1 \
		&& echo "  [OK] python3: $$(python3 --version 2>&1)" \
		|| { echo "  [FAIL] python3 not found"; FAIL=1; }; \
	pkg-config --exists liburing 2>/dev/null \
		&& echo "  [OK] liburing: $$(pkg-config --modversion liburing)" \
		|| { echo "  [FAIL] liburing-dev not found"; FAIL=1; }; \
	if [ "$$FAIL" -ne 0 ]; then \
		echo ""; \
		echo "  Mandatory packages missing. Cannot continue."; \
		exit 1; \
	fi
	@echo ""
	@# --- Optional: Rust toolchain ---
	@echo "--- Rust toolchain (optional — needed for: make rust, make rust-test) ---"
	@if command -v rustc >/dev/null 2>&1 && command -v cargo >/dev/null 2>&1; then \
		echo "  [OK] rustc: $$(rustc --version)"; \
		echo "  [OK] cargo: $$(cargo --version)"; \
	else \
		printf "  Install Rust toolchain? (y/N): "; \
		read REPLY; \
		case "$$REPLY" in \
			[yY]|[yY][eE][sS]) \
				echo "  Installing Rust..."; \
				if command -v sudo >/dev/null 2>&1; then \
					sudo apt-get install -y libclang-dev \
						&& echo "  [OK] libclang-dev installed (needed by bindgen)" \
						|| echo "  [WARN] Failed to install libclang-dev"; \
				else \
					echo "  [WARN] No sudo — skipping libclang-dev (install manually if needed)"; \
				fi; \
				if command -v curl >/dev/null 2>&1; then \
					curl --proto '=https' --tlsv1.2 -sSf https://sh.rustup.rs | sh -s -- -y \
						&& echo "  [OK] Rust installed — run: . $$HOME/.cargo/env" \
						|| echo "  [FAIL] Rust installation failed"; \
				else \
					echo "  [FAIL] curl not found — install curl or visit https://rustup.rs"; \
				fi; \
				;; \
			*) \
				echo "  [SKIP] Rust not installed"; \
				;; \
		esac; \
	fi
	@echo ""
	@# --- Optional tools status ---
	@echo "--- Optional tools (install manually as needed) ---"
	@command -v fio >/dev/null 2>&1 \
		&& echo "  [OK] fio: $$(fio --version 2>&1)" \
		|| echo "  [--] fio (apt-get install fio)"
	@command -v valgrind >/dev/null 2>&1 \
		&& echo "  [OK] valgrind: $$(valgrind --version 2>&1)" \
		|| echo "  [--] valgrind (apt-get install valgrind)"
	@command -v perf >/dev/null 2>&1 \
		&& echo "  [OK] perf: $$(perf version 2>&1 | head -1)" \
		|| echo "  [--] perf (apt-get install linux-tools-common linux-tools-$$(uname -r))"
	@command -v strace >/dev/null 2>&1 \
		&& echo "  [OK] strace: $$(strace --version 2>&1 | head -1)" \
		|| echo "  [--] strace (apt-get install strace)"
	@command -v numactl >/dev/null 2>&1 \
		&& echo "  [OK] numactl" \
		|| echo "  [--] numactl (apt-get install numactl)"
	@command -v iostat >/dev/null 2>&1 \
		&& echo "  [OK] iostat" \
		|| echo "  [--] iostat (apt-get install sysstat)"
	@command -v cppcheck >/dev/null 2>&1 \
		&& echo "  [OK] cppcheck: $$(cppcheck --version 2>&1)" \
		|| echo "  [--] cppcheck (apt-get install cppcheck)"
	@command -v bear >/dev/null 2>&1 \
		&& echo "  [OK] bear: $$(bear --version 2>&1 | head -1)" \
		|| echo "  [--] bear (apt-get install bear)"
	@echo ""
	@echo "========================================"
	@echo "Done!"
	@echo "========================================"

# Check dependencies without installing
deps-check:
	@echo "========================================"
	@echo "AuraIO Dependency Check"
	@echo "========================================"
	@echo ""
	@FAIL=0; WARN=0; \
	echo "--- Kernel ---"; \
	KMAJOR=$$(uname -r | cut -d. -f1); \
	if [ "$$KMAJOR" -lt 6 ] 2>/dev/null; then \
		echo "  [FAIL] Kernel $$(uname -r) — requires >= 6.0"; FAIL=1; \
	else \
		echo "  [OK] Kernel $$(uname -r)"; \
	fi; \
	echo ""; \
	echo "--- Mandatory (C/C++) ---"; \
	command -v gcc >/dev/null 2>&1 \
		&& echo "  [OK] gcc: $$(gcc --version | head -1)" \
		|| { echo "  [FAIL] gcc"; FAIL=1; }; \
	command -v g++ >/dev/null 2>&1 \
		&& echo "  [OK] g++: $$(g++ --version | head -1)" \
		|| { echo "  [FAIL] g++"; FAIL=1; }; \
	command -v make >/dev/null 2>&1 \
		&& echo "  [OK] make: $$(make --version | head -1)" \
		|| { echo "  [FAIL] make"; FAIL=1; }; \
	command -v pkg-config >/dev/null 2>&1 \
		&& echo "  [OK] pkg-config: $$(pkg-config --version)" \
		|| { echo "  [FAIL] pkg-config"; FAIL=1; }; \
	command -v python3 >/dev/null 2>&1 \
		&& echo "  [OK] python3: $$(python3 --version 2>&1)" \
		|| { echo "  [FAIL] python3"; FAIL=1; }; \
	pkg-config --exists liburing 2>/dev/null \
		&& echo "  [OK] liburing: $$(pkg-config --modversion liburing)" \
		|| { echo "  [FAIL] liburing-dev"; FAIL=1; }; \
	echo "  [OK] pthread (always available)"; \
	echo ""; \
	echo "--- Rust (optional) ---"; \
	command -v rustc >/dev/null 2>&1 \
		&& echo "  [OK] rustc: $$(rustc --version)" \
		|| { echo "  [--] rustc: not found"; WARN=1; }; \
	command -v cargo >/dev/null 2>&1 \
		&& echo "  [OK] cargo: $$(cargo --version)" \
		|| { echo "  [--] cargo: not found"; WARN=1; }; \
	echo ""; \
	echo "--- Optional tools ---"; \
	command -v fio >/dev/null 2>&1 \
		&& echo "  [OK] fio" || echo "  [--] fio"; \
	command -v valgrind >/dev/null 2>&1 \
		&& echo "  [OK] valgrind" || echo "  [--] valgrind"; \
	command -v perf >/dev/null 2>&1 \
		&& echo "  [OK] perf" || echo "  [--] perf"; \
	command -v strace >/dev/null 2>&1 \
		&& echo "  [OK] strace" || echo "  [--] strace"; \
	command -v numactl >/dev/null 2>&1 \
		&& echo "  [OK] numactl" || echo "  [--] numactl"; \
	command -v iostat >/dev/null 2>&1 \
		&& echo "  [OK] iostat" || echo "  [--] iostat"; \
	command -v cppcheck >/dev/null 2>&1 \
		&& echo "  [OK] cppcheck" || echo "  [--] cppcheck"; \
	command -v bear >/dev/null 2>&1 \
		&& echo "  [OK] bear" || echo "  [--] bear"; \
	echo ""; \
	if [ "$$FAIL" -ne 0 ]; then \
		echo "RESULT: FAIL — missing mandatory dependencies (run: make deps)"; \
		exit 1; \
	elif [ "$$WARN" -ne 0 ]; then \
		echo "RESULT: OK (some optional tools missing)"; \
	else \
		echo "RESULT: ALL OK"; \
	fi

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
	@echo "  make deps           Install required dependencies (sudo apt-get)"
	@echo "  make deps-check     Check dependencies without installing"
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

.PHONY: all test test-all examples install uninstall clean debug deps deps-check help \
        cpp-test cpp-examples \
        rust rust-test rust-examples rust-clean \
        tsan asan test-valgrind test-tsan test-asan test-sanitizers \
        lint lint-cppcheck lint-strict lint-clang-tidy compdb compdb-manual
