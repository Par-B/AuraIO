#!/usr/bin/env bash
set -euo pipefail

MIN_VALGRIND_VERSION="${AURA_MIN_VALGRIND_VERSION:-3.20.0}"
MIN_LIBURING_VERSION="${AURA_MIN_LIBURING_VERSION:-2.7}"
MIN_GCC_VERSION="${AURA_MIN_GCC_VERSION:-11.0}"
MIN_GXX_VERSION="${AURA_MIN_GXX_VERSION:-11.0}"
MIN_CLANG_VERSION="${AURA_MIN_CLANG_VERSION:-14.0}"
MIN_MAKE_VERSION="${AURA_MIN_MAKE_VERSION:-4.2}"
MIN_CMAKE_VERSION="${AURA_MIN_CMAKE_VERSION:-3.20.0}"
MIN_PYTHON_VERSION="${AURA_MIN_PYTHON_VERSION:-3.8.0}"
MIN_RUST_VERSION="${AURA_MIN_RUST_VERSION:-1.70.0}"
MIN_CARGO_VERSION="${AURA_MIN_CARGO_VERSION:-1.70.0}"
MIN_GDB_VERSION="${AURA_MIN_GDB_VERSION:-10.0}"
MIN_CLANG_TIDY_VERSION="${AURA_MIN_CLANG_TIDY_VERSION:-14.0}"

version_lt() {
    local a b
    a="$1"
    b="$2"
    [ "$(printf '%s\n%s\n' "$a" "$b" | sort -V | head -n1)" != "$b" ]
}

extract_version() {
    sed -E 's/[^0-9]*([0-9]+(\.[0-9]+)+).*/\1/'
}

check_cmd() {
    local cmd="$1"
    local label="$2"
    if command -v "$cmd" >/dev/null 2>&1; then
        echo "  [OK] ${label}"
        return 0
    fi
    echo "  [FAIL] ${label}"
    return 1
}

check_clang_tidy_cmd() {
    if command -v clang-tidy >/dev/null 2>&1; then
        echo "clang-tidy"
        return 0
    fi
    for v in 20 19 18 17 16 15 14; do
        if command -v "clang-tidy-$v" >/dev/null 2>&1; then
            echo "clang-tidy-$v"
            return 0
        fi
    done
    return 1
}

check_min_version() {
    local name="$1"
    local cmd="$2"
    local min="$3"
    local current
    current="$(eval "$cmd" 2>/dev/null | extract_version || true)"
    if [ -z "$current" ]; then
        echo "  [FAIL] ${name} version unreadable"
        return 1
    fi
    if version_lt "$current" "$min"; then
        echo "  [FAIL] ${name} version ${current} < ${min}"
        return 1
    fi
    echo "  [OK] ${name} version ${current} (min ${min})"
    return 0
}

echo "========================================"
echo "AuraIO Dependency Check"
echo "========================================"
echo ""

FAIL=0

echo "--- Core toolchain ---"
check_cmd gcc "gcc" || FAIL=1
check_cmd g++ "g++" || FAIL=1
check_cmd make "make" || FAIL=1
check_cmd cmake "cmake" || FAIL=1
check_cmd pkg-config "pkg-config" || FAIL=1
check_cmd python3 "python3" || FAIL=1

LIBURING_VER=""
if pkg-config --exists liburing 2>/dev/null; then
    LIBURING_VER="$(pkg-config --modversion liburing)"
elif [ -f /usr/local/include/liburing/io_uring_version.h ]; then
    _major="$(grep '#define IO_URING_VERSION_MAJOR' /usr/local/include/liburing/io_uring_version.h | awk '{print $3}')"
    _minor="$(grep '#define IO_URING_VERSION_MINOR' /usr/local/include/liburing/io_uring_version.h | awk '{print $3}')"
    LIBURING_VER="${_major}.${_minor}"
fi
if [ -z "$LIBURING_VER" ]; then
    echo "  [FAIL] liburing"
    FAIL=1
elif version_lt "$LIBURING_VER" "$MIN_LIBURING_VERSION"; then
    echo "  [FAIL] liburing version ${LIBURING_VER} < ${MIN_LIBURING_VERSION} (run: make deps)"
    FAIL=1
else
    echo "  [OK] liburing (${LIBURING_VER}, min ${MIN_LIBURING_VERSION})"
fi

if pkg-config --exists libcrypto 2>/dev/null; then
    echo "  [OK] libcrypto ($(pkg-config --modversion libcrypto))"
else
    echo "  [FAIL] libcrypto (libssl-dev)"
    FAIL=1
fi

echo ""
echo "--- Rust toolchain ---"
check_cmd rustc "rustc" || FAIL=1
check_cmd cargo "cargo" || FAIL=1

echo ""
echo "--- Supporting tools ---"
for cmd in clang gdb fio strace numactl iostat cppcheck bear perf valgrind; do
    check_cmd "$cmd" "$cmd" || FAIL=1
done
if CT_CMD="$(check_clang_tidy_cmd)"; then
    echo "  [OK] ${CT_CMD}"
else
    echo "  [FAIL] clang-tidy (or versioned clang-tidy-N)"
    FAIL=1
fi

echo ""
echo "--- Minimum versions ---"
check_min_version "gcc" "gcc -dumpfullversion -dumpversion" "$MIN_GCC_VERSION" || FAIL=1
check_min_version "g++" "g++ -dumpfullversion -dumpversion" "$MIN_GXX_VERSION" || FAIL=1
check_min_version "clang" "clang --version | head -n1" "$MIN_CLANG_VERSION" || FAIL=1
check_min_version "make" "make --version | head -n1" "$MIN_MAKE_VERSION" || FAIL=1
check_min_version "cmake" "cmake --version | head -n1" "$MIN_CMAKE_VERSION" || FAIL=1
check_min_version "python3" "python3 --version" "$MIN_PYTHON_VERSION" || FAIL=1
check_min_version "rustc" "rustc --version" "$MIN_RUST_VERSION" || FAIL=1
check_min_version "cargo" "cargo --version" "$MIN_CARGO_VERSION" || FAIL=1
check_min_version "gdb" "gdb --version | head -n1" "$MIN_GDB_VERSION" || FAIL=1
if CT_CMD="$(check_clang_tidy_cmd)"; then
    check_min_version "clang-tidy" "$CT_CMD --version | head -n1" "$MIN_CLANG_TIDY_VERSION" || FAIL=1
fi

if command -v valgrind >/dev/null 2>&1; then
    V="$(valgrind --version | sed -E 's/^valgrind-([0-9.]+).*$/\1/')"
    if version_lt "$V" "$MIN_VALGRIND_VERSION"; then
        echo "  [FAIL] valgrind version ${V} < ${MIN_VALGRIND_VERSION}"
        FAIL=1
    else
        echo "  [OK] valgrind version ${V}"
    fi
fi

echo ""
if [ "$FAIL" -ne 0 ]; then
    echo "RESULT: FAIL — missing dependencies (run: make deps)"
    exit 1
fi

echo "RESULT: ALL OK"
