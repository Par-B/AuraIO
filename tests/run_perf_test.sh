#!/bin/bash
#
# AuraIO Performance Benchmark Runner
# Portable benchmark suite with fio baseline comparison
#
# Usage: ./run_perf_test.sh [options]
#   --quick       Run quick benchmarks (5 sec each)
#   --full        Run full benchmarks (30 sec each)
#   --baseline    Run fio baseline only
#   --auraio      Run AuraIO benchmarks only
#   --compare     Run both and compare (default)
#   --with-perf   Run with perf stat (requires root/perf access)
#   --help        Show this help

set -e

# Configuration
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(dirname "$SCRIPT_DIR")"
TEST_DIR="/tmp/auraio_bench"
RESULTS_DIR="${SCRIPT_DIR}/bench_results"

# Test parameters
DURATION=10
FILE_SIZE="128M"
NUM_FILES=8
BLOCK_SIZES="4k 64k"
IO_DEPTHS="1 16 64 256"

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

# Parse arguments
RUN_BASELINE=true
RUN_AURAIO=true
USE_PERF=false
QUICK=false

while [[ $# -gt 0 ]]; do
    case $1 in
        --quick)
            QUICK=true
            DURATION=5
            shift
            ;;
        --full)
            DURATION=30
            shift
            ;;
        --baseline)
            RUN_BASELINE=true
            RUN_AURAIO=false
            shift
            ;;
        --auraio)
            RUN_BASELINE=false
            RUN_AURAIO=true
            shift
            ;;
        --compare)
            RUN_BASELINE=true
            RUN_AURAIO=true
            shift
            ;;
        --with-perf)
            USE_PERF=true
            shift
            ;;
        --help|-h)
            head -n 15 "$0" | tail -n 13
            exit 0
            ;;
        *)
            echo "Unknown option: $1"
            exit 1
            ;;
    esac
done

# ============================================================================
# Dependency Checking
# ============================================================================

check_command() {
    local cmd=$1
    local required=$2
    local install_hint=$3

    if command -v "$cmd" &> /dev/null; then
        local version=$($cmd --version 2>&1 | head -n1 || echo "unknown")
        echo -e "${GREEN}[OK]${NC} $cmd: $version"
        return 0
    else
        if [ "$required" = "required" ]; then
            echo -e "${RED}[MISSING]${NC} $cmd - $install_hint"
            return 1
        else
            echo -e "${YELLOW}[OPTIONAL]${NC} $cmd - $install_hint"
            return 0
        fi
    fi
}

check_perf_access() {
    if [ -r /proc/sys/kernel/perf_event_paranoid ]; then
        local paranoid=$(cat /proc/sys/kernel/perf_event_paranoid)
        if [ "$paranoid" -le 1 ] || [ "$(id -u)" -eq 0 ]; then
            echo -e "${GREEN}[OK]${NC} perf access: paranoid=$paranoid (sufficient)"
            return 0
        else
            echo -e "${YELLOW}[LIMITED]${NC} perf access: paranoid=$paranoid (run as root or: sudo sysctl kernel.perf_event_paranoid=1)"
            return 1
        fi
    else
        echo -e "${YELLOW}[UNKNOWN]${NC} perf access: cannot read paranoid level"
        return 1
    fi
}

check_io_uring() {
    if [ -e /proc/sys/kernel/io_uring_disabled ]; then
        local disabled=$(cat /proc/sys/kernel/io_uring_disabled 2>/dev/null || echo "0")
        if [ "$disabled" = "0" ]; then
            echo -e "${GREEN}[OK]${NC} io_uring: enabled"
            return 0
        else
            echo -e "${RED}[DISABLED]${NC} io_uring: disabled (value=$disabled)"
            return 1
        fi
    else
        # Older kernels don't have this file, try to detect via kernel version
        local kver=$(uname -r | cut -d. -f1-2)
        if awk "BEGIN {exit !($kver >= 5.1)}"; then
            echo -e "${GREEN}[OK]${NC} io_uring: kernel $kver (likely supported)"
            return 0
        else
            echo -e "${RED}[UNSUPPORTED]${NC} io_uring: kernel $kver (requires >= 5.1)"
            return 1
        fi
    fi
}

echo "============================================"
echo "AuraIO Performance Benchmark"
echo "============================================"
echo ""
echo "Checking dependencies..."
echo ""

DEPS_OK=true

# Required dependencies
check_command "gcc" "required" "apt install build-essential" || DEPS_OK=false
check_command "make" "required" "apt install build-essential" || DEPS_OK=false

# Check io_uring
check_io_uring || DEPS_OK=false

# Optional but recommended
if [ "$RUN_BASELINE" = true ]; then
    check_command "fio" "required" "apt install fio" || DEPS_OK=false
fi

# Optional profiling tools
if [ "$USE_PERF" = true ]; then
    check_command "perf" "required" "apt install linux-tools-common linux-tools-$(uname -r)" || DEPS_OK=false
    check_perf_access || USE_PERF=false
fi

# Optional analysis tools
check_command "numactl" "optional" "apt install numactl (for NUMA-aware testing)"
check_command "iostat" "optional" "apt install sysstat (for I/O statistics)"

echo ""

if [ "$DEPS_OK" = false ]; then
    echo -e "${RED}Missing required dependencies. Please install them and retry.${NC}"
    exit 1
fi

# ============================================================================
# Build AuraIO
# ============================================================================

if [ "$RUN_AURAIO" = true ]; then
    echo "Building AuraIO..."
    cd "$PROJECT_ROOT"
    make clean > /dev/null 2>&1 || true
    make -j$(nproc) > /dev/null 2>&1
    make -C tests perf_bench > /dev/null 2>&1
    echo -e "${GREEN}Build complete${NC}"
    echo ""
fi

# ============================================================================
# Setup Test Environment
# ============================================================================

setup_test_env() {
    echo "Setting up test environment..."

    # Create test directory
    mkdir -p "$TEST_DIR"
    mkdir -p "$RESULTS_DIR"

    # Drop caches for consistent results (requires root)
    if [ "$(id -u)" -eq 0 ]; then
        sync
        echo 3 > /proc/sys/vm/drop_caches 2>/dev/null || true
        echo -e "${GREEN}Dropped page cache${NC}"
    else
        echo -e "${YELLOW}Skipping cache drop (not root)${NC}"
    fi

    # Record system info
    {
        echo "=== System Info ==="
        echo "Date: $(date -Iseconds)"
        echo "Hostname: $(hostname)"
        echo "Kernel: $(uname -r)"
        echo "CPUs: $(nproc)"
        echo "Memory: $(free -h | awk '/^Mem:/{print $2}')"
        echo ""
        echo "=== CPU Info ==="
        lscpu | grep -E "Model name|CPU\(s\)|Thread|Core|Socket|MHz"
        echo ""
        echo "=== Storage ==="
        df -h "$TEST_DIR" | tail -1
        echo ""
    } > "$RESULTS_DIR/system_info.txt"

    echo "Test directory: $TEST_DIR"
    echo "Results directory: $RESULTS_DIR"
    echo ""
}

cleanup_test_env() {
    echo "Cleaning up test files..."
    rm -rf "$TEST_DIR"
}

trap cleanup_test_env EXIT

setup_test_env

# ============================================================================
# FIO Baseline Tests
# ============================================================================

run_fio_baseline() {
    echo "============================================"
    echo "Running FIO Baseline Tests"
    echo "============================================"
    echo ""

    local fio_results="$RESULTS_DIR/fio_results.txt"
    > "$fio_results"

    # Random Read Test (comparable to AuraIO throughput test)
    echo "Running: Random Read (4K blocks, iodepth=64)..."
    fio --name=randread_4k \
        --ioengine=io_uring \
        --rw=randread \
        --bs=4k \
        --direct=1 \
        --size="$FILE_SIZE" \
        --numjobs=4 \
        --iodepth=64 \
        --runtime="$DURATION" \
        --time_based \
        --group_reporting \
        --directory="$TEST_DIR" \
        --output-format=terse \
        2>/dev/null | tee -a "$fio_results"

    echo ""
    echo "Running: Random Read (64K blocks, iodepth=256)..."
    fio --name=randread_64k \
        --ioengine=io_uring \
        --rw=randread \
        --bs=64k \
        --direct=1 \
        --size="$FILE_SIZE" \
        --numjobs=4 \
        --iodepth=256 \
        --runtime="$DURATION" \
        --time_based \
        --group_reporting \
        --directory="$TEST_DIR" \
        --output-format=terse \
        2>/dev/null | tee -a "$fio_results"

    echo ""
    echo "Running: Sequential Read (64K blocks)..."
    fio --name=seqread_64k \
        --ioengine=io_uring \
        --rw=read \
        --bs=64k \
        --direct=1 \
        --size="$FILE_SIZE" \
        --numjobs=4 \
        --iodepth=32 \
        --runtime="$DURATION" \
        --time_based \
        --group_reporting \
        --directory="$TEST_DIR" \
        --output-format=terse \
        2>/dev/null | tee -a "$fio_results"

    echo ""
    echo "Running: Mixed Read/Write (70/30)..."
    fio --name=mixed_rw \
        --ioengine=io_uring \
        --rw=randrw \
        --rwmixread=70 \
        --bs=32k \
        --direct=1 \
        --size="$FILE_SIZE" \
        --numjobs=4 \
        --iodepth=128 \
        --runtime="$DURATION" \
        --time_based \
        --group_reporting \
        --directory="$TEST_DIR" \
        --output-format=terse \
        2>/dev/null | tee -a "$fio_results"

    echo ""
    echo "Running: Latency Test (4K, iodepth=1)..."
    fio --name=latency_4k \
        --ioengine=io_uring \
        --rw=randread \
        --bs=4k \
        --direct=1 \
        --size="$FILE_SIZE" \
        --numjobs=1 \
        --iodepth=1 \
        --runtime="$DURATION" \
        --time_based \
        --group_reporting \
        --directory="$TEST_DIR" \
        --lat_percentiles=1 \
        --output-format=json \
        2>/dev/null > "$RESULTS_DIR/fio_latency.json"

    # Parse and display summary
    echo ""
    echo "=== FIO Baseline Summary ==="
    echo ""

    # Parse terse output: field 7 = read IOPS, field 48 = read bandwidth KB/s
    echo "Test               IOPS        Bandwidth    Latency(avg)"
    echo "----               ----        ---------    ------------"

    while IFS=';' read -r name rw bs iodepth size runtime lat_min lat_max lat_mean lat_stdev bw bw_min bw_max bw_agg bw_mean bw_dev iops iops_min iops_max iops_mean iops_stddev rest; do
        if [ -n "$name" ] && [ "$name" != "terse_version_3" ]; then
            printf "%-18s %-11s %-12s %s\n" "$name" "${iops:-N/A}" "${bw:-N/A} KB/s" "${lat_mean:-N/A} us"
        fi
    done < "$fio_results" 2>/dev/null || echo "(parse error - check $fio_results)"

    echo ""
}

# ============================================================================
# AuraIO Benchmark Tests
# ============================================================================

run_auraio_benchmark() {
    echo "============================================"
    echo "Running AuraIO Benchmark Tests"
    echo "============================================"
    echo ""

    local auraio_results="$RESULTS_DIR/auraio_results.txt"
    local perf_bench="$SCRIPT_DIR/perf_bench"

    if [ ! -x "$perf_bench" ]; then
        echo -e "${RED}perf_bench not found. Build with: make -C tests perf_bench${NC}"
        return 1
    fi

    # Run with or without perf
    local runner=""
    if [ "$USE_PERF" = true ]; then
        runner="perf stat -e cycles,instructions,cache-misses,context-switches,cpu-migrations"
    fi

    {
        echo "=== AuraIO Benchmark Results ==="
        echo "Date: $(date -Iseconds)"
        echo "Duration: ${DURATION}s per test"
        echo ""

        echo "--- Throughput Test ---"
        $runner "$perf_bench" throughput "$DURATION"
        echo ""

        echo "--- Latency Test ---"
        $runner "$perf_bench" latency "$DURATION"
        echo ""

        echo "--- Scalability Test ---"
        $runner "$perf_bench" scalability $((DURATION / 2))
        echo ""

        echo "--- Mixed Workload Test ---"
        $runner "$perf_bench" mixed "$DURATION"
        echo ""

        echo "--- Buffer Pool Test ---"
        $runner "$perf_bench" buffer "$DURATION"
        echo ""

        echo "--- Syscall Batching Test ---"
        $runner "$perf_bench" syscall "$DURATION"
        echo ""

    } 2>&1 | tee "$auraio_results"

    echo ""
}

# ============================================================================
# Generate Comparison Report
# ============================================================================

generate_report() {
    echo "============================================"
    echo "Benchmark Comparison Report"
    echo "============================================"
    echo ""

    local report="$RESULTS_DIR/benchmark_report.txt"

    {
        echo "AuraIO vs FIO Performance Comparison"
        echo "====================================="
        echo ""
        echo "Test Configuration:"
        echo "  Duration: ${DURATION}s per test"
        echo "  File size: $FILE_SIZE"
        echo "  Test directory: $TEST_DIR"
        echo ""
        cat "$RESULTS_DIR/system_info.txt"
        echo ""
        echo "Note: FIO tests use io_uring engine for fair comparison."
        echo "AuraIO adds adaptive tuning (AIMD) overhead but auto-optimizes."
        echo ""
        echo "Results saved to: $RESULTS_DIR/"
        echo ""
    } > "$report"

    cat "$report"
}

# ============================================================================
# Main Execution
# ============================================================================

if [ "$RUN_BASELINE" = true ]; then
    run_fio_baseline
fi

if [ "$RUN_AURAIO" = true ]; then
    run_auraio_benchmark
fi

if [ "$RUN_BASELINE" = true ] && [ "$RUN_AURAIO" = true ]; then
    generate_report
fi

echo ""
echo -e "${GREEN}Benchmark complete!${NC}"
echo "Results saved to: $RESULTS_DIR/"
