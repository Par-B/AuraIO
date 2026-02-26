#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")/../.." && pwd)"
cd "$ROOT_DIR"

if ! command -v apt-get >/dev/null 2>&1; then
    echo "[FAIL] make deps currently supports apt-based Linux only." >&2
    exit 1
fi

if [ "$(id -u)" -eq 0 ]; then
    SUDO=""
elif command -v sudo >/dev/null 2>&1; then
    SUDO="sudo"
else
    echo "[FAIL] sudo is required to install dependencies." >&2
    exit 1
fi

run_root() {
    if [ -n "$SUDO" ]; then
        $SUDO "$@"
    else
        "$@"
    fi
}

version_lt() {
    local a b
    a="$1"
    b="$2"
    [ "$(printf '%s\n%s\n' "$a" "$b" | sort -V | head -n1)" != "$b" ]
}

extract_version() {
    sed -E 's/[^0-9]*([0-9]+(\.[0-9]+)+).*/\1/'
}

check_version_or_fail() {
    local name="$1"
    local current="$2"
    local min="$3"
    if [ -z "$current" ]; then
        echo "[FAIL] ${name}: could not determine version" >&2
        exit 1
    fi
    if version_lt "$current" "$min"; then
        echo "[FAIL] ${name}: ${current} < minimum ${min}" >&2
        exit 1
    fi
    echo "[OK] ${name}: ${current} (min ${min})"
}

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
MIN_VALGRIND_VERSION="${AURA_MIN_VALGRIND_VERSION:-3.20.0}"
TARGET_VALGRIND_VERSION="${AURA_VALGRIND_TARGET_VERSION:-3.26.0}"
BUILD_VALGRIND_FROM_SOURCE="${AURA_BUILD_VALGRIND_FROM_SOURCE:-1}"
MIN_LIBURING_VERSION="${AURA_MIN_LIBURING_VERSION:-2.7}"
TARGET_LIBURING_VERSION="${AURA_LIBURING_TARGET_VERSION:-2.9}"

echo "========================================"
echo "AuraIO Dependency Installer"
echo "========================================"

echo "--- Installing base packages ---"
BASE_PACKAGES=(
    build-essential
    gcc
    g++
    make
    pkg-config
    liburing-dev
    libssl-dev
    python3
    python3-pip
    curl
    wget
    ca-certificates
    git
    cmake
    ninja-build
    clang
    clang-tidy
    clang-tools
    lldb
    gdb
    gdbserver
    libclang-dev
    llvm
    llvm-dev
    lld
    rustc
    cargo
    fio
    valgrind
    strace
    numactl
    sysstat
    cppcheck
    bear
    bison
    flex
)
run_root apt-get update
run_root apt-get install -y "${BASE_PACKAGES[@]}"

echo "--- Installing kernel perf tools (best effort) ---"
run_root apt-get install -y linux-perf 2>/dev/null || true
run_root apt-get install -y "linux-tools-$(uname -r)" 2>/dev/null || true
run_root apt-get install -y linux-tools-common linux-tools-generic 2>/dev/null || true

if ! command -v rustup >/dev/null 2>&1; then
    echo "--- Installing rustup (non-interactive) ---"
    curl --proto '=https' --tlsv1.2 -sSf https://sh.rustup.rs | sh -s -- -y --profile minimal --default-toolchain stable
fi

if [ -f "$HOME/.cargo/env" ]; then
    # shellcheck source=/dev/null
    . "$HOME/.cargo/env"
fi

if command -v rustup >/dev/null 2>&1; then
    rustup toolchain install stable >/dev/null 2>&1 || true
fi

current_valgrind_version() {
    local bin
    bin="$(command -v valgrind || true)"
    if [ -z "$bin" ]; then
        echo ""
        return
    fi
    "$bin" --version | sed -E 's/^valgrind-([0-9.]+).*$/\1/'
}

find_clang_tidy_cmd() {
    if command -v clang-tidy >/dev/null 2>&1; then
        echo "clang-tidy"
        return
    fi
    for v in 20 19 18 17 16 15 14; do
        if command -v "clang-tidy-$v" >/dev/null 2>&1; then
            echo "clang-tidy-$v"
            return
        fi
    done
    echo ""
}

VALGRIND_VER="$(current_valgrind_version)"
if [ -z "$VALGRIND_VER" ]; then
    echo "[WARN] valgrind is not installed after apt install."
elif version_lt "$VALGRIND_VER" "$MIN_VALGRIND_VERSION"; then
    if [ "$BUILD_VALGRIND_FROM_SOURCE" = "1" ]; then
        echo "--- Building Valgrind ${TARGET_VALGRIND_VERSION} from source (current: ${VALGRIND_VER}) ---"
        TMP_DIR="/tmp/aura-valgrind-build"
        rm -rf "$TMP_DIR"
        mkdir -p "$TMP_DIR"
        cd "$TMP_DIR"
        wget -q "https://sourceware.org/pub/valgrind/valgrind-${TARGET_VALGRIND_VERSION}.tar.bz2"
        tar -xjf "valgrind-${TARGET_VALGRIND_VERSION}.tar.bz2"
        cd "valgrind-${TARGET_VALGRIND_VERSION}"
        ./configure --prefix=/usr/local >/dev/null
        make -j"$(nproc)" >/dev/null
        run_root make install >/dev/null
        cd "$ROOT_DIR"
        rm -rf "$TMP_DIR"
    else
        echo "[WARN] valgrind ${VALGRIND_VER} < ${MIN_VALGRIND_VERSION}; source build is disabled."
    fi
fi

current_liburing_version() {
    if pkg-config --exists liburing 2>/dev/null; then
        pkg-config --modversion liburing
    elif [ -f /usr/local/include/liburing/io_uring_version.h ]; then
        local major minor
        major="$(grep '#define IO_URING_VERSION_MAJOR' /usr/local/include/liburing/io_uring_version.h | awk '{print $3}')"
        minor="$(grep '#define IO_URING_VERSION_MINOR' /usr/local/include/liburing/io_uring_version.h | awk '{print $3}')"
        echo "${major}.${minor}"
    else
        echo ""
    fi
}

LIBURING_VER="$(current_liburing_version)"
if [ -z "$LIBURING_VER" ]; then
    echo "[WARN] liburing is not installed after apt install."
elif version_lt "$LIBURING_VER" "$MIN_LIBURING_VERSION"; then
    echo "--- Building liburing ${TARGET_LIBURING_VERSION} from source (current: ${LIBURING_VER}) ---"
    TMP_DIR="/tmp/aura-liburing-build"
    rm -rf "$TMP_DIR"
    git clone --depth 1 --branch "liburing-${TARGET_LIBURING_VERSION}" \
        https://github.com/axboe/liburing.git "$TMP_DIR"
    cd "$TMP_DIR"
    ./configure --prefix=/usr/local
    make -j"$(nproc)"
    run_root make install
    run_root ldconfig
    cd "$ROOT_DIR"
    rm -rf "$TMP_DIR"
    LIBURING_VER="$(current_liburing_version)"
    echo "[OK] liburing upgraded to ${LIBURING_VER}"
else
    echo "[OK] liburing: ${LIBURING_VER} (min ${MIN_LIBURING_VERSION})"
fi

echo "--- Verifying minimum tool versions ---"
GCC_VER="$(gcc -dumpfullversion -dumpversion 2>/dev/null || true)"
GXX_VER="$(g++ -dumpfullversion -dumpversion 2>/dev/null || true)"
CLANG_VER="$(clang --version 2>/dev/null | head -n1 | extract_version || true)"
MAKE_VER="$(make --version 2>/dev/null | head -n1 | extract_version || true)"
CMAKE_VER="$(cmake --version 2>/dev/null | head -n1 | extract_version || true)"
PY_VER="$(python3 --version 2>&1 | extract_version || true)"
RUST_VER="$(rustc --version 2>/dev/null | extract_version || true)"
CARGO_VER="$(cargo --version 2>/dev/null | extract_version || true)"
GDB_VER="$(gdb --version 2>/dev/null | head -n1 | extract_version || true)"
CLANG_TIDY_CMD="$(find_clang_tidy_cmd)"
CLANG_TIDY_VER="$(
    if [ -n "$CLANG_TIDY_CMD" ]; then
        "$CLANG_TIDY_CMD" --version 2>/dev/null | head -n1 | extract_version || true
    else
        echo ""
    fi
)"
VALGRIND_VER="$(current_valgrind_version)"

check_version_or_fail "gcc" "$GCC_VER" "$MIN_GCC_VERSION"
check_version_or_fail "g++" "$GXX_VER" "$MIN_GXX_VERSION"
check_version_or_fail "clang" "$CLANG_VER" "$MIN_CLANG_VERSION"
check_version_or_fail "make" "$MAKE_VER" "$MIN_MAKE_VERSION"
check_version_or_fail "cmake" "$CMAKE_VER" "$MIN_CMAKE_VERSION"
check_version_or_fail "python3" "$PY_VER" "$MIN_PYTHON_VERSION"
check_version_or_fail "rustc" "$RUST_VER" "$MIN_RUST_VERSION"
check_version_or_fail "cargo" "$CARGO_VER" "$MIN_CARGO_VERSION"
check_version_or_fail "gdb" "$GDB_VER" "$MIN_GDB_VERSION"
check_version_or_fail "clang-tidy" "$CLANG_TIDY_VER" "$MIN_CLANG_TIDY_VERSION"
check_version_or_fail "valgrind" "$VALGRIND_VER" "$MIN_VALGRIND_VERSION"

echo "--- Installed tool versions ---"
command -v gcc >/dev/null 2>&1 && echo "gcc: $(gcc --version | head -n1)"
command -v g++ >/dev/null 2>&1 && echo "g++: $(g++ --version | head -n1)"
command -v pkg-config >/dev/null 2>&1 && echo "pkg-config: $(pkg-config --version)"
pkg-config --exists liburing 2>/dev/null && echo "liburing: $(pkg-config --modversion liburing)"
pkg-config --exists libcrypto 2>/dev/null && echo "libcrypto: $(pkg-config --modversion libcrypto)"
command -v clang >/dev/null 2>&1 && echo "clang: $(clang --version | head -n1)"
if [ -n "$CLANG_TIDY_CMD" ]; then
    echo "clang-tidy: $("$CLANG_TIDY_CMD" --version | head -n1)"
fi
command -v cargo >/dev/null 2>&1 && echo "cargo: $(cargo --version)"
command -v rustc >/dev/null 2>&1 && echo "rustc: $(rustc --version)"
command -v valgrind >/dev/null 2>&1 && echo "valgrind: $(valgrind --version)"
command -v fio >/dev/null 2>&1 && echo "fio: $(fio --version)"
command -v perf >/dev/null 2>&1 && echo "perf: $(perf version 2>/dev/null | head -n1 || true)"
command -v cppcheck >/dev/null 2>&1 && echo "cppcheck: $(cppcheck --version 2>&1)"

echo ""
echo "========================================"
echo "Dependency install complete"
echo "========================================"
