#!/bin/bash
#
# AuraIO Performance Analysis Suite
# Laptop-safe benchmark with FIO comparison and perf profiling
#
# Usage: ./run_analysis.sh [options]
#   --quick       3 sec per test (fast sanity check)
#   --standard    5 sec per test (default, good for laptops)
#   --full        10 sec per test (more stable numbers)
#   --skip-fio    Skip FIO baseline tests
#   --skip-perf   Skip perf stat collection
#   --help        Show this help

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(dirname "$SCRIPT_DIR")"
RESULTS_DIR="${SCRIPT_DIR}/bench_results"
REPORT_FILE="${RESULTS_DIR}/analysis_report.txt"
TEST_DIR="/tmp/auraio_bench"

# Defaults (laptop-safe)
DURATION=5
RUN_FIO=true
RUN_PERF=true
LABEL="standard"

while [[ $# -gt 0 ]]; do
    case $1 in
        --quick)     DURATION=3; LABEL="quick"; shift ;;
        --standard)  DURATION=5; LABEL="standard"; shift ;;
        --full)      DURATION=10; LABEL="full"; shift ;;
        --skip-fio)  RUN_FIO=false; shift ;;
        --skip-perf) RUN_PERF=false; shift ;;
        --help|-h)
            head -n 11 "$0" | tail -n 9
            exit 0 ;;
        *) echo "Unknown option: $1"; exit 1 ;;
    esac
done

# Colors
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
CYAN='\033[0;36m'
NC='\033[0m'

mkdir -p "$RESULTS_DIR"
mkdir -p "$TEST_DIR"

cleanup() { rm -rf "$TEST_DIR"; }
trap cleanup EXIT

# ============================================================================
# System info
# ============================================================================
collect_system_info() {
    local outfile="$RESULTS_DIR/system_info.txt"
    {
        echo "=== System Info ==="
        echo "Date: $(date -Iseconds)"
        echo "Hostname: $(hostname)"
        echo "Kernel: $(uname -r)"
        echo "CPUs: $(nproc)"
        echo "Memory: $(free -h | awk '/^Mem:/{print $2}')"
        echo ""
        echo "=== CPU Info ==="
        lscpu | grep -E "Model name|CPU\(s\)|Thread|Core|Socket|MHz" || true
        echo ""
        echo "=== Storage ==="
        df -h "$TEST_DIR" | tail -1
        echo ""
        echo "=== io_uring ==="
        if [ -e /proc/sys/kernel/io_uring_disabled ]; then
            echo "io_uring_disabled=$(cat /proc/sys/kernel/io_uring_disabled)"
        else
            echo "io_uring: likely supported (kernel $(uname -r))"
        fi
    } > "$outfile"
    cat "$outfile"
}

# ============================================================================
# Build
# ============================================================================
build_benchmark() {
    echo -e "${CYAN}Building AuraIO...${NC}"
    cd "$PROJECT_ROOT"
    make -j"$(nproc)" > /dev/null 2>&1
    make -C tests perf_bench > /dev/null 2>&1
    echo -e "${GREEN}Build complete${NC}"
    cd "$SCRIPT_DIR"
}

# ============================================================================
# FIO baseline (single-job, laptop-friendly)
# ============================================================================
run_fio_tests() {
    echo ""
    echo "============================================"
    echo "FIO Baseline (io_uring engine, ${DURATION}s each)"
    echo "============================================"

    local fio_summary="$RESULTS_DIR/fio_summary.txt"
    > "$fio_summary"

    run_fio_test() {
        local name=$1 bs=$2 rw=$3 depth=$4 njobs=${5:-1}
        echo -n "  $name ... "
        local json_file="$RESULTS_DIR/fio_${name}.json"
        fio --name="$name" \
            --ioengine=io_uring \
            --rw="$rw" \
            --bs="$bs" \
            --direct=1 \
            --size=128M \
            --numjobs="$njobs" \
            --iodepth="$depth" \
            --runtime="$DURATION" \
            --time_based \
            --group_reporting \
            --directory="$TEST_DIR" \
            --lat_percentiles=1 \
            --output-format=json \
            2>/dev/null > "$json_file"

        python3 -c "
import json, sys
d = json.load(open('$json_file'))['jobs'][0]
r = d.get('read', {})
w = d.get('write', {})
rio = r.get('iops', 0)
wio = w.get('iops', 0)
rbw = r.get('bw', 0) / 1024  # KB/s -> MB/s
wbw = w.get('bw', 0) / 1024
rp99 = r.get('clat_ns', {}).get('percentile', {}).get('99.000000', 0) / 1000
wp99 = w.get('clat_ns', {}).get('percentile', {}).get('99.000000', 0) / 1000
ravg = r.get('lat_ns', {}).get('mean', 0) / 1000
if rio > 0:
    print(f'{rio:.0f} IOPS, {rbw:.1f} MB/s, avg={ravg:.0f}us, p99={rp99:.0f}us')
    print(f'$name|{rio:.0f}|{rbw:.1f}|{ravg:.0f}|{rp99:.0f}', file=open('$fio_summary', 'a'))
if wio > 0:
    print(f'  + writes: {wio:.0f} IOPS, {wbw:.1f} MB/s')
" 2>&1
    }

    # Laptop-safe: single numjobs for most tests
    run_fio_test "randread_4k"   "4k"  "randread" 64
    run_fio_test "randread_64k"  "64k" "randread" 64
    run_fio_test "seqread_64k"   "64k" "read"     32
    run_fio_test "latency_4k"    "4k"  "randread" 1
    run_fio_test "mixed_rw_32k"  "32k" "randrw"   64
}

# ============================================================================
# AuraIO benchmarks (run individually to avoid the all-mode hang)
# ============================================================================
run_auraio_tests() {
    echo ""
    echo "============================================"
    echo "AuraIO Benchmarks (${DURATION}s each)"
    echo "============================================"

    local perf_bench="$SCRIPT_DIR/perf_bench"
    local auraio_out="$RESULTS_DIR/auraio_full.txt"
    > "$auraio_out"

    local perf_prefix=""
    if [ "$RUN_PERF" = true ] && command -v perf &>/dev/null; then
        local paranoid
        paranoid=$(cat /proc/sys/kernel/perf_event_paranoid 2>/dev/null || echo "4")
        if [ "$paranoid" -le 2 ] || [ "$(id -u)" -eq 0 ]; then
            perf_prefix="perf stat -e context-switches,cpu-migrations,page-faults"
        else
            echo -e "  ${YELLOW}perf unavailable (paranoid=$paranoid), skipping perf stat${NC}"
            RUN_PERF=false
        fi
    fi

    # Run each benchmark individually (avoids the sequential hang in "all" mode)
    for bench in throughput latency scalability mixed syscall buffer; do
        local bench_dur=$DURATION
        # Scalability runs multiple sub-tests, give it shorter per-depth duration
        if [ "$bench" = "scalability" ]; then
            bench_dur=$((DURATION > 2 ? DURATION / 2 : 2))
        fi

        echo ""
        echo "--- $bench (${bench_dur}s) ---"

        if [ "$RUN_PERF" = true ] && [ -n "$perf_prefix" ]; then
            $perf_prefix "$perf_bench" "$bench" "$bench_dur" 2>&1 | tee -a "$auraio_out"
        else
            "$perf_bench" "$bench" "$bench_dur" 2>&1 | tee -a "$auraio_out"
        fi
    done

    # Also run strace for syscall efficiency metrics
    if command -v strace &>/dev/null; then
        echo ""
        echo "--- Syscall Trace (3s) ---"
        strace -fc "$perf_bench" syscall 3 2>&1 | grep -E "io_uring|total" | tee -a "$auraio_out"
    fi
}

# ============================================================================
# Parse and generate comparison report
# ============================================================================
generate_report() {
    echo ""
    echo "============================================"
    echo "Generating Analysis Report"
    echo "============================================"

    local auraio_out="$RESULTS_DIR/auraio_full.txt"
    local fio_summary="$RESULTS_DIR/fio_summary.txt"

    python3 - "$auraio_out" "$fio_summary" "$RESULTS_DIR/system_info.txt" "$REPORT_FILE" "$DURATION" "$LABEL" << 'PYEOF'
import re, sys, os

auraio_file = sys.argv[1]
fio_file = sys.argv[2]
sysinfo_file = sys.argv[3]
report_file = sys.argv[4]
duration = sys.argv[5]
label = sys.argv[6]

lines = open(auraio_file).read() if os.path.exists(auraio_file) else ""
fio_lines = open(fio_file).readlines() if os.path.exists(fio_file) else []
sysinfo = open(sysinfo_file).read() if os.path.exists(sysinfo_file) else ""

report = []
report.append("=" * 70)
report.append("AuraIO Performance Analysis Report")
report.append("=" * 70)
report.append("")
report.append(sysinfo.strip())
report.append("")
report.append(f"Test profile: {label} ({duration}s per benchmark)")
report.append("")

# Parse AuraIO results
report.append("-" * 70)
report.append("AURAIO BENCHMARK RESULTS")
report.append("-" * 70)

# Throughput
tp_match = re.search(r'Throughput Benchmark.*?Throughput:\s+([\d.]+)\s+MB/s.*?IOPS:\s+([\d.]+).*?P99 latency:\s+(\d+)\s+us.*?Optimal depth:\s+(\d+)', lines, re.DOTALL)
if tp_match:
    report.append(f"\n  Throughput (64K random read, max inflight):")
    report.append(f"    IOPS:          {int(float(tp_match.group(2))):,}")
    report.append(f"    Bandwidth:     {float(tp_match.group(1)):.1f} MB/s")
    report.append(f"    P99 latency:   {int(tp_match.group(3)):,} us")
    report.append(f"    AIMD depth:    {tp_match.group(4)}")

# Latency
lat_match = re.search(r'Latency Benchmark.*?IOPS:\s+([\d.]+).*?Min latency:\s+([\d.]+)\s+us.*?Avg latency:\s+([\d.]+)\s+us.*?Max latency:\s+([\d.]+)\s+us.*?P99 latency:\s+(\d+)\s+us', lines, re.DOTALL)
if lat_match:
    report.append(f"\n  Latency (4K serial, depth=1):")
    report.append(f"    IOPS:          {int(float(lat_match.group(1))):,}")
    report.append(f"    Min:           {float(lat_match.group(2)):.1f} us")
    report.append(f"    Avg:           {float(lat_match.group(3)):.1f} us")
    report.append(f"    Max:           {float(lat_match.group(4)):.1f} us")
    report.append(f"    P99:           {int(lat_match.group(5)):,} us")

# Scalability
scale_matches = re.findall(r'^(\d+)\s+([\d.]+)\s+([\d.]+)\s+(\d+)\s*$', lines, re.MULTILINE)
if scale_matches:
    report.append(f"\n  Scalability (64K, varying queue depth):")
    report.append(f"    {'Depth':<10} {'IOPS':<12} {'MB/s':<12} {'P99(us)':<10}")
    report.append(f"    {'-----':<10} {'----':<12} {'----':<12} {'-------':<10}")
    for depth, iops, mbps, p99 in scale_matches:
        report.append(f"    {depth:<10} {int(float(iops)):>8,}    {float(mbps):>8.1f}    {int(p99):>6,}")

# Mixed
mix_match = re.search(r'Mixed Workload.*?Reads:\s+(\d+).*?Writes:\s+(\d+).*?Fsyncs:\s+(\d+).*?Total ops:\s+(\d+).*?Throughput:\s+([\d.]+)\s+MB/s.*?IOPS:\s+([\d.]+)', lines, re.DOTALL)
if mix_match:
    report.append(f"\n  Mixed Workload (70R/25W/5F, 32K):")
    report.append(f"    Total IOPS:    {int(float(mix_match.group(6))):,}")
    report.append(f"    Bandwidth:     {float(mix_match.group(5)):.1f} MB/s")
    report.append(f"    Reads:         {int(mix_match.group(1)):,}")
    report.append(f"    Writes:        {int(mix_match.group(2)):,}")
    report.append(f"    Fsyncs:        {int(mix_match.group(3)):,}")

# Syscall batching
sc_match = re.search(r'Syscall Batching.*?Operations:\s+(\d+).*?IOPS:\s+([\d.]+).*?Throughput:\s+([\d.]+)\s+MB/s.*?Optimal depth:\s+(\d+)', lines, re.DOTALL)
if sc_match:
    report.append(f"\n  Syscall Batching (4K, depth=256):")
    report.append(f"    IOPS:          {int(float(sc_match.group(2))):,}")
    report.append(f"    Bandwidth:     {float(sc_match.group(3)):.1f} MB/s")
    report.append(f"    AIMD depth:    {sc_match.group(4)}")

# Buffer pool
buf_match = re.search(r'Buffer Pool.*?Alloc rate:\s+([\d.]+)\s+ops.*?Combined:\s+([\d.]+)\s+ops.*?Per-thread:\s+([\d.]+)\s+ops', lines, re.DOTALL)
if buf_match:
    report.append(f"\n  Buffer Pool (4 threads, mixed sizes):")
    report.append(f"    Alloc rate:    {int(float(buf_match.group(1))):,} ops/sec")
    report.append(f"    Combined:      {int(float(buf_match.group(2))):,} ops/sec")
    report.append(f"    Per-thread:    {int(float(buf_match.group(3))):,} ops/sec")

# io_uring syscall trace
uring_match = re.search(r'\d+\s+\d+\s+(\d+)\s+io_uring_enter', lines)
if uring_match:
    n = int(uring_match.group(1))
    report.append(f"\n  Syscall Efficiency (3s trace):")
    report.append(f"    io_uring_enter calls: {n:,}")
    if sc_match:
        ops = int(sc_match.group(1))
        report.append(f"    Ops/syscall:   ~{ops // n}")

# FIO comparison
if fio_lines:
    report.append("")
    report.append("-" * 70)
    report.append("FIO BASELINE COMPARISON (io_uring engine)")
    report.append("-" * 70)
    report.append(f"\n    {'Test':<20} {'IOPS':<10} {'MB/s':<10} {'Avg(us)':<10} {'P99(us)':<10}")
    report.append(f"    {'----':<20} {'----':<10} {'----':<10} {'-------':<10} {'-------':<10}")
    for line in fio_lines:
        parts = line.strip().split('|')
        if len(parts) == 5:
            name, iops, mbps, avg, p99 = parts
            report.append(f"    {name:<20} {iops:>8}  {mbps:>8}  {avg:>8}  {p99:>8}")

# Key findings
report.append("")
report.append("-" * 70)
report.append("KEY FINDINGS")
report.append("-" * 70)

findings = []

# Compare AuraIO latency vs FIO latency
fio_lat = None
for line in fio_lines:
    if 'latency_4k' in line:
        parts = line.strip().split('|')
        if len(parts) == 5:
            fio_lat = (float(parts[3]), float(parts[4]))

if lat_match and fio_lat:
    a_avg = float(lat_match.group(3))
    f_avg = fio_lat[0]
    if f_avg > 0:
        overhead = ((a_avg - f_avg) / f_avg) * 100
        findings.append(f"  - Serial latency overhead vs FIO: {overhead:+.0f}% (AuraIO {a_avg:.0f}us vs FIO {f_avg:.0f}us avg)")
        if abs(overhead) < 30:
            findings.append(f"    => GOOD: Low abstraction overhead for adaptive tuning layer")
        elif overhead > 0:
            findings.append(f"    => NOTE: Overhead from AIMD controller + buffer pool management")

# Scalability analysis
if scale_matches:
    iops_vals = [(int(d), float(i)) for d, i, _, _ in scale_matches]
    peak = max(iops_vals, key=lambda x: x[1])
    findings.append(f"  - Peak throughput at depth={peak[0]}: {int(peak[1]):,} IOPS")
    d4 = next((i for d, i in iops_vals if d == 4), None)
    d128 = next((i for d, i in iops_vals if d == 128), None)
    if d4 and d128:
        scale_ratio = d128 / d4
        findings.append(f"  - Scaling ratio (depth 4->128): {scale_ratio:.2f}x")

# Throughput finding
if tp_match:
    tp = float(tp_match.group(1))
    findings.append(f"  - Max throughput: {tp:.0f} MB/s (64K random reads)")

# Compare AuraIO 64K throughput vs FIO 64K throughput
fio_64k = None
for line in fio_lines:
    if 'randread_64k' in line:
        parts = line.strip().split('|')
        if len(parts) == 5:
            fio_64k = float(parts[2])
if tp_match and fio_64k and fio_64k > 0:
    a_tp = float(tp_match.group(1))
    ratio = a_tp / fio_64k
    findings.append(f"  - AuraIO vs FIO 64K throughput: {ratio:.2f}x ({a_tp:.0f} vs {fio_64k:.0f} MB/s)")

# Buffer pool finding
if buf_match:
    combined = float(buf_match.group(2))
    findings.append(f"  - Buffer pool: {combined/1e6:.1f}M ops/sec combined (TLS cache effective)")

# AIMD convergence
aimd_m = re.search(r'Optimal depth:\s+(\d+)', lines)
if aimd_m:
    depth = int(aimd_m.group(1))
    if depth == 4096:
        findings.append(f"  - AIMD converged to max depth ({depth}) — storage not saturated")
    else:
        findings.append(f"  - AIMD converged to depth {depth}")

# Syscall efficiency finding
if uring_match and sc_match:
    n = int(uring_match.group(1))
    ops = int(sc_match.group(1))
    findings.append(f"  - Syscall efficiency: {ops//n} ops per io_uring_enter call")

if not findings:
    findings.append("  (insufficient data for analysis)")

report.extend(findings)
report.append("")
report.append("=" * 70)

output = '\n'.join(report)
print(output)
with open(report_file, 'w') as f:
    f.write(output + '\n')

print(f"\nReport saved to: {report_file}")
PYEOF
}

# ============================================================================
# Main
# ============================================================================

echo "============================================"
echo "AuraIO Performance Analysis"
echo "Profile: $LABEL ($DURATION sec per test)"
echo "============================================"
echo ""

collect_system_info
build_benchmark

if [ "$RUN_FIO" = true ] && command -v fio &>/dev/null; then
    run_fio_tests
elif [ "$RUN_FIO" = true ]; then
    echo -e "${YELLOW}fio not found, skipping baseline${NC}"
    RUN_FIO=false
fi

run_auraio_tests
generate_report

echo ""
echo -e "${GREEN}Analysis complete!${NC} Results in: $RESULTS_DIR/"
