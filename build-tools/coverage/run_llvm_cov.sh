#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
OUT_DIR="${1:-$ROOT/tests/coverage}"
MIN_LLVM_VERSION=18
MIN_LINE_COVERAGE="${MIN_LINE_COVERAGE:-0}"

# Auto-detect LLVM toolchain: prefer versioned binaries (clang-N), fall back to
# unversioned (clang).  Honour CC_BIN / LLVM_COV_BIN / LLVM_PROFDATA_BIN overrides.
detect_llvm() {
    if [[ -n "${CC_BIN:-}" ]]; then
        # User override — extract version from the provided compiler.
        local ver
        ver=$("$CC_BIN" --version 2>/dev/null | grep -oP 'version \K[0-9]+' | head -1) || true
        LLVM_COV_BIN="${LLVM_COV_BIN:-llvm-cov-${ver:-}}"
        LLVM_PROFDATA_BIN="${LLVM_PROFDATA_BIN:-llvm-profdata-${ver:-}}"
        return
    fi

    # Search for the highest installed versioned clang >= MIN_LLVM_VERSION.
    local found_ver=""
    for candidate in $(compgen -c clang- 2>/dev/null | grep -oP '^clang-\K[0-9]+$' | sort -rn | uniq); do
        if (( candidate >= MIN_LLVM_VERSION )); then
            found_ver="$candidate"
            break
        fi
    done

    if [[ -n "$found_ver" ]]; then
        CC_BIN="clang-${found_ver}"
        LLVM_COV_BIN="${LLVM_COV_BIN:-llvm-cov-${found_ver}}"
        LLVM_PROFDATA_BIN="${LLVM_PROFDATA_BIN:-llvm-profdata-${found_ver}}"
    elif command -v clang >/dev/null 2>&1; then
        CC_BIN="clang"
        LLVM_COV_BIN="${LLVM_COV_BIN:-llvm-cov}"
        LLVM_PROFDATA_BIN="${LLVM_PROFDATA_BIN:-llvm-profdata}"
    else
        echo "No clang >= ${MIN_LLVM_VERSION} found" >&2
        exit 1
    fi
}

detect_llvm

# Verify minimum version.
clang_ver=$("$CC_BIN" --version 2>/dev/null | grep -oP 'version \K[0-9]+' | head -1) || true
if [[ -z "$clang_ver" ]] || (( clang_ver < MIN_LLVM_VERSION )); then
    echo "Requires clang >= ${MIN_LLVM_VERSION}, found version ${clang_ver:-unknown}" >&2
    exit 1
fi

for bin in "$CC_BIN" "$LLVM_COV_BIN" "$LLVM_PROFDATA_BIN"; do
    if ! command -v "$bin" >/dev/null 2>&1; then
        echo "Missing required tool: $bin" >&2
        exit 1
    fi
done

mkdir -p "$OUT_DIR/profraw" "$OUT_DIR/html"
rm -f "$OUT_DIR/profraw"/*.profraw "$OUT_DIR"/coverage.profdata "$OUT_DIR"/report.txt \
    "$OUT_DIR"/summary.json "$OUT_DIR"/lcov.info "$OUT_DIR"/line_coverage.txt

LIB_CFLAGS="-Wall -Wextra -Wshadow -Wpedantic -Wstrict-prototypes -Wmissing-declarations -std=c11 -O0 -g -fPIC -fvisibility=hidden -Iengine/include -Iengine/src -fprofile-instr-generate -fcoverage-mapping"
TEST_CFLAGS="-Wall -Wextra -Wpedantic -std=c11 -g -O0 -I../engine/include -I../engine/src -fprofile-instr-generate -fcoverage-mapping"
# Rebuild engine library and tests with coverage instrumentation.
# Use `engine` instead of `all` so example/binding builds do not inherit
# instrumentation CFLAGS that are scoped for top-level library paths.
make -C "$ROOT" clean >/dev/null
make -C "$ROOT" -j4 engine CC="$CC_BIN" CFLAGS="$LIB_CFLAGS" >/dev/null
make -C "$ROOT/tests" clean >/dev/null
make -C "$ROOT/tests" \
    CC="$CC_BIN" CFLAGS="$TEST_CFLAGS" \
    test_engine test_buffer test_ring test_stats test_ring_select test_concurrency_stress test_coverage >/dev/null

# Build test_otel separately (needs exporter sources and include path)
"$CC_BIN" $TEST_CFLAGS -I"$ROOT/engine/include" -I"$ROOT/integrations/opentelemetry/C" \
    "$ROOT/tests/test_otel.c" "$ROOT/integrations/opentelemetry/C/aura_otel.c" "$ROOT/integrations/opentelemetry/C/aura_otel_push.c" \
    -o "$ROOT/tests/test_otel" "$ROOT/engine/lib/libaura.a" -luring -lpthread \
    -fprofile-instr-generate -fcoverage-mapping >/dev/null

TEST_BINS=(
    test_engine
    test_buffer
    test_ring
    test_stats
    test_ring_select
    test_concurrency_stress
    test_otel
    test_coverage
)

for t in "${TEST_BINS[@]}"; do
    LLVM_PROFILE_FILE="$OUT_DIR/profraw/${t}-%p.profraw" "$ROOT/tests/$t" >/dev/null
done

$LLVM_PROFDATA_BIN merge -sparse "$OUT_DIR"/profraw/*.profraw -o "$OUT_DIR/coverage.profdata"

COV_ARGS=("$ROOT/tests/${TEST_BINS[0]}" "-instr-profile=$OUT_DIR/coverage.profdata")
for t in "${TEST_BINS[@]:1}"; do
    COV_ARGS+=("-object" "$ROOT/tests/$t")
done

IGNORE_RE='.*/(tests|exporters|tools|build-tools|bindings/rust|usr/include|include/c\+\+)/.*'

$LLVM_COV_BIN report "${COV_ARGS[@]}" -ignore-filename-regex="$IGNORE_RE" > "$OUT_DIR/report.txt"
$LLVM_COV_BIN export "${COV_ARGS[@]}" -summary-only -ignore-filename-regex="$IGNORE_RE" > "$OUT_DIR/summary.json"
$LLVM_COV_BIN export "${COV_ARGS[@]}" -format=lcov -ignore-filename-regex="$IGNORE_RE" > "$OUT_DIR/lcov.info"
$LLVM_COV_BIN show "${COV_ARGS[@]}" -format=html -output-dir="$OUT_DIR/html" \
    -ignore-filename-regex="$IGNORE_RE" >/dev/null

line_cov=$(perl -0777 -ne 'if(/"totals"\s*:\s*\{.*?"lines"\s*:\s*\{[^}]*"percent"\s*:\s*([0-9.]+)/s){print $1}' "$OUT_DIR/summary.json")
if [[ -z "$line_cov" ]]; then
    echo "Failed to parse total line coverage from $OUT_DIR/summary.json" >&2
    exit 1
fi

printf '%s\n' "$line_cov" > "$OUT_DIR/line_coverage.txt"
printf 'Total line coverage: %.2f%%\n' "$line_cov"
printf 'Coverage artifacts:\n'
printf '  %s\n' "$OUT_DIR/report.txt" "$OUT_DIR/summary.json" "$OUT_DIR/lcov.info" "$OUT_DIR/html/index.html"

if awk "BEGIN { exit !($MIN_LINE_COVERAGE > 0) }"; then
    if awk "BEGIN { exit !($line_cov + 0 < $MIN_LINE_COVERAGE + 0) }"; then
        echo "Coverage check failed: ${line_cov}% < ${MIN_LINE_COVERAGE}%" >&2
        exit 1
    fi
    echo "Coverage check passed: ${line_cov}% >= ${MIN_LINE_COVERAGE}%"
fi
