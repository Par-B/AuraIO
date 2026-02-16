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
TEST_DIR="/tmp/aura_bench"

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
        --dir)       TEST_DIR="$2"; shift 2 ;;
        --help|-h)
            head -n 11 "$0" | tail -n 9
            exit 0 ;;
        *) echo "Unknown option: $1"; exit 1 ;;
    esac
done

# Export so perf_bench picks it up
export AURA_BENCH_DIR="$TEST_DIR"

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
# FIO baseline (tuned, io_uring engine)
# ============================================================================
run_fio_tests() {
    echo ""
    echo "============================================"
    echo "FIO Tuned Baseline (io_uring, ${DURATION}s each)"
    echo "============================================"

    local fio_summary="$RESULTS_DIR/fio_summary.txt"
    > "$fio_summary"

    run_fio_test() {
        local name=$1 bs=$2 rw=$3 depth=$4 njobs=$5 nrfiles=$6 fsize=$7
        shift 7
        local extra_args="$*"
        echo -n "  $name (depth=$depth, ${nrfiles}x files) ... "
        local json_file="$RESULTS_DIR/fio_${name}.json"
        fio --name="$name" \
            --ioengine=io_uring \
            --rw="$rw" \
            --bs="$bs" \
            --direct=1 \
            --size="$fsize" \
            --numjobs="$njobs" \
            --iodepth="$depth" \
            --nrfiles="$nrfiles" \
            --runtime="$DURATION" \
            --time_based \
            --group_reporting \
            --directory="$TEST_DIR" \
            --lat_percentiles=1 \
            --output-format=json \
            $extra_args \
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
rp99 = r.get('lat_ns', {}).get('percentile', {}).get('99.000000', 0) / 1000
wp99 = w.get('lat_ns', {}).get('percentile', {}).get('99.000000', 0) / 1000
ravg = r.get('lat_ns', {}).get('mean', 0) / 1000
if rio > 0:
    print(f'{rio:.0f} IOPS, {rbw:.1f} MB/s, avg={ravg:.0f}us, p99={rp99:.0f}us')
    print(f'$name|{rio:.0f}|{rbw:.1f}|{ravg:.0f}|{rp99:.0f}', file=open('$fio_summary', 'a'))
if wio > 0:
    print(f'  + writes: {wio:.0f} IOPS, {wbw:.1f} MB/s')
" 2>&1
    }

    # Matched parameters: 8 files x 128MB = 1GB working set (matches AuraIO's setup)
    # Depths match AuraIO's max_inflight for each test
    run_fio_test "randread_64k"  "64k" "randread" 256  1  8  1024M
    run_fio_test "latency_4k"    "4k"  "randread" 1    1  8  1024M
    run_fio_test "mixed_rw_32k"  "32k" "randrw"   128  1  8  1024M  --rwmixread=70 --fsync=6
}

# ============================================================================
# AuraIO benchmarks (run individually to avoid the all-mode hang)
# ============================================================================
run_aura_tests() {
    echo ""
    echo "============================================"
    echo "AuraIO Benchmarks (${DURATION}s each)"
    echo "============================================"

    local perf_bench="$SCRIPT_DIR/perf_bench"
    local aura_out="$RESULTS_DIR/aura_full.txt"
    > "$aura_out"

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

        # Timeout: bench duration * 8 (scalability has 6 sub-tests) + headroom.
        # Prevents io_uring + perf stat deadlocks from hanging the pipeline.
        local bench_timeout=$((bench_dur * 8 + 15))

        echo ""
        echo "--- $bench (${bench_dur}s) ---"

        # Skip perf stat for syscall benchmark — io_uring_wait_cqe_timeout
        # deadlocks under ptrace/perf on modern kernels (6.5+).
        # Use || true to prevent set -e from killing the script on timeout.
        if [ "$RUN_PERF" = true ] && [ -n "$perf_prefix" ] && [ "$bench" != "syscall" ]; then
            timeout "$bench_timeout" $perf_prefix "$perf_bench" "$bench" "$bench_dur" 2>&1 | tee -a "$aura_out" || true
        else
            timeout "$bench_timeout" "$perf_bench" "$bench" "$bench_dur" 2>&1 | tee -a "$aura_out" || true
        fi
        local rc=${PIPESTATUS[0]}
        if [ $rc -eq 124 ]; then
            echo "  [warn] $bench timed out after ${bench_timeout}s (killed)" | tee -a "$aura_out"
        fi
    done

    # NOTE: strace syscall tracing removed — ptrace deadlocks with io_uring
    # on modern kernels.  The "syscall" benchmark in perf_bench already prints
    # batching efficiency (ops per io_uring_enter) without ptrace overhead.
}

# ============================================================================
# AuraIO apples-to-apples run (ring_count=1 for FIO-comparable results)
# ============================================================================
run_aura_apples() {
    echo ""
    echo "============================================"
    echo "AuraIO Apples-to-Apples (single ring, ${DURATION}s each)"
    echo "============================================"

    local perf_bench="$SCRIPT_DIR/perf_bench"
    local apples_out="$RESULTS_DIR/aura_apples.txt"
    > "$apples_out"

    export AURA_BENCH_APPLES=1

    # Only run FIO-compared benchmarks (throughput, latency, mixed)
    for bench in throughput latency mixed; do
        local bench_dur=$DURATION
        local bench_timeout=$((bench_dur * 8 + 15))

        echo ""
        echo "--- $bench [matched] (${bench_dur}s) ---"

        timeout "$bench_timeout" "$perf_bench" "$bench" "$bench_dur" 2>&1 | tee -a "$apples_out" || true
        local rc=${PIPESTATUS[0]}
        if [ $rc -eq 124 ]; then
            echo "  [warn] $bench timed out after ${bench_timeout}s (killed)" | tee -a "$apples_out"
        fi
    done

    unset AURA_BENCH_APPLES
}

# ============================================================================
# Parse and generate comparison report
# ============================================================================
generate_report() {
    echo ""
    echo "============================================"
    echo "Generating Analysis Report"
    echo "============================================"

    local aura_out="$RESULTS_DIR/aura_full.txt"
    local aura_apples_out="$RESULTS_DIR/aura_apples.txt"
    local fio_summary="$RESULTS_DIR/fio_summary.txt"

    python3 - "$aura_out" "$fio_summary" "$RESULTS_DIR/system_info.txt" "$REPORT_FILE" "$DURATION" "$LABEL" "$aura_apples_out" << 'PYEOF'
import re, sys, os

aura_file = sys.argv[1]
fio_file = sys.argv[2]
sysinfo_file = sys.argv[3]
report_file = sys.argv[4]
duration = sys.argv[5]
label = sys.argv[6]
apples_file = sys.argv[7] if len(sys.argv) > 7 else ""

lines = open(aura_file).read() if os.path.exists(aura_file) else ""
apples_lines = open(apples_file).read() if apples_file and os.path.exists(apples_file) else ""
fio_lines = open(fio_file).readlines() if os.path.exists(fio_file) else []
sysinfo = open(sysinfo_file).read() if os.path.exists(sysinfo_file) else ""

# --- Helper: parse AuraIO output into a dict of matches ---
def parse_aura(text):
    d = {}
    d['tp'] = re.search(
        r'Throughput Benchmark.*?Throughput:\s+([\d.]+)\s+MB/s.*?IOPS:\s+([\d.]+).*?P99 latency:\s+(\d+)\s+us.*?Optimal depth:\s+(\d+)',
        text, re.DOTALL)
    d['lat'] = re.search(
        r'Latency Benchmark.*?IOPS:\s+([\d.]+).*?Min latency:\s+([\d.]+)\s+us.*?Avg latency:\s+([\d.]+)\s+us.*?Max latency:\s+([\d.]+)\s+us.*?P99 latency:\s+(\d+)\s+us',
        text, re.DOTALL)
    d['mix'] = re.search(
        r'Mixed Workload.*?Reads:\s+(\d+).*?Writes:\s+(\d+).*?Fsyncs:\s+(\d+).*?Total ops:\s+(\d+).*?Throughput:\s+([\d.]+)\s+MB/s.*?IOPS:\s+([\d.]+)',
        text, re.DOTALL)
    d['sc'] = re.search(
        r'Syscall Batching.*?Operations:\s+(\d+).*?IOPS:\s+([\d.]+).*?Throughput:\s+([\d.]+)\s+MB/s.*?Optimal depth:\s+(\d+)',
        text, re.DOTALL)
    d['buf'] = re.search(
        r'Buffer Pool.*?Alloc rate:\s+([\d.]+)\s+ops.*?Combined:\s+([\d.]+)\s+ops.*?Per-thread:\s+([\d.]+)\s+ops',
        text, re.DOTALL)
    d['scale'] = re.findall(r'^(\d+)\s+([\d.]+)\s+([\d.]+)\s+(\d+)\s*$', text, re.MULTILINE)
    return d

auto = parse_aura(lines)
apples = parse_aura(apples_lines) if apples_lines else {}

# Parse FIO data
fio_data = {}
for line in fio_lines:
    parts = line.strip().split('|')
    if len(parts) == 5:
        name, iops, mbps, avg, p99 = parts
        fio_data[name] = {
            'iops': float(iops), 'mbps': float(mbps),
            'avg': float(avg), 'p99': float(p99)
        }

report = []
report.append("=" * 70)
report.append("AuraIO Performance Analysis Report")
report.append("=" * 70)
report.append("")
report.append(sysinfo.strip())
report.append("")
report.append(f"Test profile: {label} ({duration}s per benchmark)")
report.append("")

# --- AuraIO standalone results (auto-tuned mode) ---
report.append("-" * 70)
report.append("AURAIO BENCHMARK RESULTS (auto-tuned)")
report.append("-" * 70)

tp = auto.get('tp')
if tp:
    report.append(f"\n  Throughput (64K random read, auto rings):")
    report.append(f"    IOPS:          {int(float(tp.group(2))):,}")
    report.append(f"    Bandwidth:     {float(tp.group(1)):,.1f} MB/s")
    report.append(f"    P99 latency:   {int(tp.group(3)):,} us")
    report.append(f"    AIMD depth:    {tp.group(4)}")

lat = auto.get('lat')
if lat:
    report.append(f"\n  Latency (4K serial, depth=1):")
    report.append(f"    IOPS:          {int(float(lat.group(1))):,}")
    report.append(f"    Min:           {float(lat.group(2)):.1f} us")
    report.append(f"    Avg:           {float(lat.group(3)):.1f} us")
    report.append(f"    Max:           {float(lat.group(4)):.1f} us")
    report.append(f"    P99:           {int(lat.group(5)):,} us")

scale = auto.get('scale', [])
if scale:
    report.append(f"\n  Scalability (64K, varying queue depth):")
    report.append(f"    {'Depth':<10} {'IOPS':<12} {'MB/s':<12} {'P99(us)':<10}")
    report.append(f"    {'-----':<10} {'----':<12} {'----':<12} {'-------':<10}")
    for depth, iops, mbps, p99 in scale:
        report.append(f"    {depth:<10} {int(float(iops)):>8,}    {float(mbps):>8,.1f}    {int(p99):>6,}")

mix = auto.get('mix')
if mix:
    report.append(f"\n  Mixed Workload (70R/25W/5F, 32K, auto rings):")
    report.append(f"    Total IOPS:    {int(float(mix.group(6))):,}")
    report.append(f"    Bandwidth:     {float(mix.group(5)):,.1f} MB/s")
    report.append(f"    Reads:         {int(mix.group(1)):,}")
    report.append(f"    Writes:        {int(mix.group(2)):,}")
    report.append(f"    Fsyncs:        {int(mix.group(3)):,}")

sc = auto.get('sc')
if sc:
    report.append(f"\n  Syscall Batching (4K, depth=256):")
    report.append(f"    IOPS:          {int(float(sc.group(2))):,}")
    report.append(f"    Bandwidth:     {float(sc.group(3)):,.1f} MB/s")
    report.append(f"    AIMD depth:    {sc.group(4)}")

buf = auto.get('buf')
if buf:
    report.append(f"\n  Buffer Pool (4 threads, mixed sizes):")
    report.append(f"    Alloc rate:    {int(float(buf.group(1))):,} ops/sec")
    report.append(f"    Combined:      {int(float(buf.group(2))):,} ops/sec")
    report.append(f"    Per-thread:    {int(float(buf.group(3))):,} ops/sec")

# --- Helper: build comparison rows ---
def build_comparison_rows(fio, aura_data):
    """Build (name, f_iops, f_mbps, a_iops, a_mbps, pct) rows."""
    rows = []
    f = fio.get('randread_64k')
    t = aura_data.get('tp')
    if t and f:
        a_iops = int(float(t.group(2)))
        a_mbps = float(t.group(1))
        pct = ((a_mbps - f['mbps']) / f['mbps'] * 100) if f['mbps'] > 0 else 0
        rows.append(('64K randread', int(f['iops']), f['mbps'], a_iops, a_mbps, pct))
    f = fio.get('latency_4k')
    l = aura_data.get('lat')
    if l and f:
        a_iops = int(float(l.group(1)))
        a_mbps = a_iops * 4 / 1024
        pct = ((a_iops - f['iops']) / f['iops'] * 100) if f['iops'] > 0 else 0
        rows.append(('4K serial read', int(f['iops']), f['mbps'], a_iops, a_mbps, pct))
    f = fio.get('mixed_rw_32k')
    m = aura_data.get('mix')
    if m and f:
        a_iops = int(float(m.group(6)))
        a_mbps = float(m.group(5))
        pct = ((a_mbps - f['mbps']) / f['mbps'] * 100) if f['mbps'] > 0 else 0
        rows.append(('Mixed R/W (32K)', int(f['iops']), f['mbps'], a_iops, a_mbps, pct))
    return rows

def build_latency_rows(fio, aura_data):
    """Build latency comparison rows."""
    f = fio.get('latency_4k')
    l = aura_data.get('lat')
    if not (l and f):
        return None
    return {
        'a_avg': float(l.group(3)), 'a_p99': int(l.group(5)),
        'f_avg': f['avg'], 'f_p99': f['p99']
    }

def emit_comparison_table(report, title, subtitle, fio_label, aura_label, rows, latency):
    report.append("")
    report.append("-" * 70)
    report.append(title)
    if subtitle:
        report.append(f"  {subtitle}")
    report.append("-" * 70)
    report.append("")
    report.append(f"  {'Test':<22} {fio_label:^21}   {aura_label:^21}   {'Delta':>7}")
    report.append(f"  {'':<22} {'IOPS':>9} {'MB/s':>10}   {'IOPS':>9} {'MB/s':>10}   {'':>7}")
    report.append(f"  {'-'*22} {'-'*9} {'-'*10}   {'-'*9} {'-'*10}   {'-'*7}")
    for name, f_iops, f_mbps, a_iops, a_mbps, pct in rows:
        report.append(f"  {name:<22} {f_iops:>9,} {f_mbps:>10,.1f}   {a_iops:>9,} {a_mbps:>10,.1f}   {pct:>+6.0f}%")
    if latency:
        d = latency
        report.append("")
        report.append(f"  {'Latency (4K, depth=1)':<22} {fio_label:^13}   {aura_label:^13}   {'Delta':>7}")
        report.append(f"  {'-'*22} {'-'*13}   {'-'*13}   {'-'*7}")
        avg_pct = ((d['a_avg'] - d['f_avg']) / d['f_avg'] * 100) if d['f_avg'] > 0 else 0
        p99_pct = ((d['a_p99'] - d['f_p99']) / d['f_p99'] * 100) if d['f_p99'] > 0 else 0
        report.append(f"  {'  Avg latency (us)':<22} {d['f_avg']:>13,.0f}   {d['a_avg']:>13,.0f}   {avg_pct:>+6.0f}%")
        report.append(f"  {'  P99 latency (us)':<22} {int(d['f_p99']):>13,}   {d['a_p99']:>13,}   {p99_pct:>+6.0f}%")

# --- Comparison 1: Apples-to-Apples ---
if fio_data and apples:
    rows = build_comparison_rows(fio_data, apples)
    latency = build_latency_rows(fio_data, apples)
    if rows:
        emit_comparison_table(report,
            "APPLES-TO-APPLES COMPARISON",
            "Both: single io_uring ring, matched iodepth, 8x128MB files, O_DIRECT",
            "FIO", "AuraIO (1 ring)",
            rows, latency)

# --- Comparison 2: FIO Tuned vs AuraIO Auto ---
if fio_data and auto:
    rows = build_comparison_rows(fio_data, auto)
    latency = build_latency_rows(fio_data, auto)
    if rows:
        emit_comparison_table(report,
            "FIO (TUNED) vs AURAIO (AUTO-TUNED)",
            "FIO: fixed iodepth, single ring | AuraIO: AIMD-tuned, auto multi-ring",
            "FIO (Tuned)", "AuraIO (Auto)",
            rows, latency)

# --- Key findings ---
report.append("")
report.append("-" * 70)
report.append("KEY FINDINGS")
report.append("-" * 70)

findings = []

# Latency overhead (use apples-to-apples for fairest comparison)
a2a_lat = apples.get('lat') if apples else None
f_lat = fio_data.get('latency_4k')
if a2a_lat and f_lat:
    a_avg = float(a2a_lat.group(3))
    f_avg = f_lat['avg']
    if f_avg > 0:
        overhead = ((a_avg - f_avg) / f_avg) * 100
        findings.append(f"  - Latency overhead vs FIO (matched): {overhead:+.0f}% ({a_avg:.0f}us vs {f_avg:.0f}us avg)")
        if abs(overhead) < 30:
            findings.append(f"    => Low abstraction overhead for adaptive tuning layer")

# Throughput: apples-to-apples
a2a_tp = apples.get('tp') if apples else None
f_64k = fio_data.get('randread_64k')
if a2a_tp and f_64k and f_64k['mbps'] > 0:
    a_tp = float(a2a_tp.group(1))
    ratio = a_tp / f_64k['mbps']
    findings.append(f"  - 64K throughput (matched): {ratio:.2f}x FIO ({a_tp:,.0f} vs {f_64k['mbps']:,.0f} MB/s)")

# Throughput: auto-tuned advantage
auto_tp = auto.get('tp')
if auto_tp and f_64k and f_64k['mbps'] > 0:
    a_tp_auto = float(auto_tp.group(1))
    ratio_auto = a_tp_auto / f_64k['mbps']
    findings.append(f"  - 64K throughput (auto-tuned): {ratio_auto:.2f}x FIO ({a_tp_auto:,.0f} vs {f_64k['mbps']:,.0f} MB/s)")
    if ratio_auto > 1.1:
        findings.append(f"    => Auto multi-ring + AIMD batching outperforms tuned FIO")

# Scalability
if scale:
    iops_vals = [(int(d), float(i)) for d, i, _, _ in scale]
    peak = max(iops_vals, key=lambda x: x[1])
    findings.append(f"  - Peak throughput at depth={peak[0]}: {int(peak[1]):,} IOPS")
    d4 = next((i for d, i in iops_vals if d == 4), None)
    d128 = next((i for d, i in iops_vals if d == 128), None)
    if d4 and d128:
        findings.append(f"  - Scaling ratio (depth 4->128): {d128/d4:.2f}x")

# Standalone throughput (no FIO)
if auto_tp and not f_64k:
    findings.append(f"  - Max throughput: {float(auto_tp.group(1)):,.0f} MB/s (64K random reads)")

# Buffer pool
if buf:
    findings.append(f"  - Buffer pool: {float(buf.group(2))/1e6:.1f}M ops/sec combined (TLS cache effective)")

# AIMD convergence
aimd_m = re.search(r'Optimal depth:\s+(\d+)', lines)
if aimd_m:
    d = int(aimd_m.group(1))
    if d == 4096:
        findings.append(f"  - AIMD converged to max depth ({d}) — storage not saturated")
    else:
        findings.append(f"  - AIMD converged to depth {d}")

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

run_aura_tests

# Run apples-to-apples (ring_count=1) if FIO comparison is enabled
if [ "$RUN_FIO" = true ]; then
    run_aura_apples
fi

generate_report

echo ""
echo -e "${GREEN}Analysis complete!${NC} Results in: $RESULTS_DIR/"
