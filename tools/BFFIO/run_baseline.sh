#!/bin/bash
# BFFIO vs FIO baseline comparison
# Usage: ./run_baseline.sh [--quick|--standard|--full] [--skip-fio] [--iterations=N] [--keep]
#
# Runs matched workloads on both FIO and BFFIO, parses JSON output,
# computes performance deltas, and generates a comparison report.
#
# Outputs: tools/BFFIO/baseline_results/comparison_report.txt

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
cd "$SCRIPT_DIR"

# ─── Defaults ───────────────────────────────────────────────────────────────

DURATION=5
SIZE_SMALL="128M"
SIZE_LARGE="256M"
MODE="standard"
SKIP_FIO=0
ITERATIONS=1
KEEP=0
TEST_DIR="/tmp/BFFIO_baseline_$$"
RESULTS_DIR="$SCRIPT_DIR/baseline_results"
BFFIO="$SCRIPT_DIR/BFFIO"

# ─── Argument parsing ──────────────────────────────────────────────────────

for arg in "$@"; do
    case "$arg" in
        --quick)
            MODE="quick"
            DURATION=2
            SIZE_SMALL="64M"
            SIZE_LARGE="64M"
            ;;
        --standard)
            MODE="standard"
            DURATION=5
            ;;
        --full)
            MODE="full"
            DURATION=15
            ;;
        --skip-fio)
            SKIP_FIO=1
            ;;
        --iterations=*)
            ITERATIONS="${arg#*=}"
            ;;
        --keep)
            KEEP=1
            ;;
        --help|-h)
            echo "Usage: $0 [--quick|--standard|--full] [--skip-fio] [--iterations=N] [--keep]"
            echo ""
            echo "  --quick       2s per test, 64M files"
            echo "  --standard    5s per test, 128M/256M files (default)"
            echo "  --full        15s per test, 128M/256M files"
            echo "  --skip-fio    Reuse previous FIO results"
            echo "  --iterations=N  Run each workload N times and average (default: 1)"
            echo "  --keep        Keep test directory after completion"
            exit 0
            ;;
        *)
            echo "Unknown argument: $arg" >&2
            exit 1
            ;;
    esac
done

# ─── Dependency checks ─────────────────────────────────────────────────────

HAS_FIO=1
HAS_PYTHON=1
HAS_JQ=1

if ! command -v fio &>/dev/null; then
    echo "Warning: fio not found, skipping FIO runs" >&2
    HAS_FIO=0
    SKIP_FIO=1
fi
if ! command -v python3 &>/dev/null; then
    echo "Error: python3 is required for JSON parsing" >&2
    exit 1
fi
if ! command -v jq &>/dev/null; then
    echo "Warning: jq not found (not required, but useful for debugging)" >&2
    HAS_JQ=0
fi

if [ ! -x "$BFFIO" ]; then
    echo "Error: BFFIO binary not found at $BFFIO" >&2
    echo "Run 'make' first to build BFFIO" >&2
    exit 1
fi

# ─── Workload definitions ──────────────────────────────────────────────────
# Format: name|rw|bs|iodepth|numjobs|size[|extra_args]

WORKLOADS=(
    "randread_4k|randread|4k|64|1|${SIZE_SMALL}"
    "randread_64k|randread|64k|256|1|${SIZE_SMALL}"
    "seqread_1m|read|1m|32|1|${SIZE_LARGE}"
    "randwrite_4k|randwrite|4k|64|1|${SIZE_SMALL}"
    "mixed_70_30|randrw|4k|128|1|${SIZE_SMALL}|--rwmixread=70"
)

# ─── Helper functions ───────────────────────────────────────────────────────

log() {
    echo "[$(date '+%H:%M:%S')] $*" >&2
}

# Create (or re-create) test files and attempt to drop caches
prepare_test_dir() {
    rm -rf "$TEST_DIR"
    mkdir -p "$TEST_DIR"
    sync
    # Drop caches if running as root (best-effort)
    if [ -w /proc/sys/vm/drop_caches ]; then
        echo 3 > /proc/sys/vm/drop_caches 2>/dev/null || true
    fi
}

# Cleanup on exit unless --keep
cleanup() {
    if [ "$KEEP" -eq 0 ]; then
        rm -rf "$TEST_DIR"
    else
        log "Keeping test directory: $TEST_DIR"
    fi
}
trap cleanup EXIT

# Run FIO for a single workload
run_fio() {
    local name="$1" rw="$2" bs="$3" depth="$4" njobs="$5" size="$6"
    shift 6
    local extra_args=("$@")
    local outfile="$RESULTS_DIR/fio_${name}.json"

    local fio_args=(
        --name="$name"
        --ioengine=io_uring
        --rw="$rw"
        --bs="$bs"
        --direct=1
        --size="$size"
        --iodepth="$depth"
        --numjobs="$njobs"
        --runtime="$DURATION"
        --time_based
        --group_reporting
        --directory="$TEST_DIR"
        --output-format=json
    )

    # Append extra args (e.g. --rwmixread=70)
    for ea in "${extra_args[@]}"; do
        [ -n "$ea" ] && fio_args+=("$ea")
    done

    fio "${fio_args[@]}" > "$outfile" 2>/dev/null
}

# Run BFFIO for a single workload
run_BFFIO() {
    local name="$1" rw="$2" bs="$3" depth="$4" njobs="$5" size="$6"
    shift 6
    local extra_args=("$@")
    local outfile="$RESULTS_DIR/BFFIO_${name}.json"

    local BFFIO_args=(
        --name="$name"
        --rw="$rw"
        --bs="$bs"
        --direct=1
        --size="$size"
        --iodepth="$depth"
        --numjobs="$njobs"
        --runtime="$DURATION"
        --time_based
        --group_reporting
        --directory="$TEST_DIR"
        --output-format=json
    )

    for ea in "${extra_args[@]}"; do
        [ -n "$ea" ] && BFFIO_args+=("$ea")
    done

    "$BFFIO" "${BFFIO_args[@]}" > "$outfile" 2>/dev/null
}

# Extract metrics from a JSON results file using python3
# Outputs: iops bw_bytes_sec lat_avg_ns lat_p99_ns
extract_metrics() {
    local json_file="$1"
    python3 -c "
import json, sys

data = json.load(open(sys.argv[1]))
job = data['jobs'][0]

r = job.get('read', {})
w = job.get('write', {})

# Sum IOPS and BW across read+write (covers mixed workloads)
iops = r.get('iops', 0) + w.get('iops', 0)
bw   = r.get('bw_bytes', r.get('bw', 0) * 1024) + w.get('bw_bytes', w.get('bw', 0) * 1024)

# Latency: prefer read direction for mixed workloads
# Use lat_ns for mean, clat_ns for percentiles (FIO puts percentiles in clat_ns)
dir = r if r.get('total_ios', 0) > 0 else w

lat = dir.get('lat_ns', {})
clat = dir.get('clat_ns', {})

avg_ns = lat.get('mean', 0) or clat.get('mean', 0)
p = clat.get('percentile', lat.get('percentile', {}))
p99_ns = p.get('99.000000', 0)

print(f'{iops:.2f} {bw:.0f} {avg_ns:.0f} {p99_ns:.0f}')
" "$json_file"
}

# Compute percentage delta: ((new - old) / old) * 100
compute_delta() {
    local fio_val="$1" BFFIO_val="$2"
    python3 -c "
fio_v = float('$fio_val')
bf_v  = float('$BFFIO_val')
if fio_v == 0:
    print('N/A')
else:
    delta = ((bf_v - fio_v) / fio_v) * 100.0
    print(f'{delta:+.1f}%')
"
}

# Format a number with commas
fmt_num() {
    python3 -c "print(f'{float(\"$1\"):,.0f}')"
}

# Format bytes/sec as MB/s
fmt_bw() {
    python3 -c "print(f'{float(\"$1\") / (1024*1024):.1f} MB/s')"
}

# Format nanoseconds as microseconds
fmt_lat() {
    python3 -c "
ns = float('$1')
if ns >= 1000000:
    print(f'{ns/1000000:.1f} ms')
else:
    print(f'{ns/1000:.0f} us')
"
}

# Average multiple iteration result files into one
# Usage: average_iterations prefix name count
# Reads prefix_name_iter{0..count-1}.json, writes prefix_name.json
average_iterations() {
    local prefix="$1" name="$2" count="$3"
    local outfile="$RESULTS_DIR/${prefix}_${name}.json"

    if [ "$count" -eq 1 ]; then
        cp "$RESULTS_DIR/${prefix}_${name}_iter0.json" "$outfile"
        return
    fi

    python3 -c "
import json, sys, copy

prefix, name, count = sys.argv[1], sys.argv[2], int(sys.argv[3])
results_dir = '$RESULTS_DIR'

files = [f'{results_dir}/{prefix}_{name}_iter{i}.json' for i in range(count)]
datasets = [json.load(open(f)) for f in files]

# Start from the first result as the template
avg = copy.deepcopy(datasets[0])
job = avg['jobs'][0]

for direction in ['read', 'write']:
    d = job.get(direction, {})
    if not d:
        continue
    # Average numeric fields
    for key in ['iops', 'bw_bytes', 'bw']:
        vals = [ds['jobs'][0].get(direction, {}).get(key, 0) for ds in datasets]
        d[key] = sum(vals) / count
    # Average latency
    lat = d.get('lat_ns', {})
    if lat:
        for key in ['mean', 'min', 'max', 'stddev']:
            vals = [ds['jobs'][0].get(direction, {}).get('lat_ns', {}).get(key, 0) for ds in datasets]
            lat[key] = sum(vals) / count
        pct = lat.get('percentile', {})
        for pkey in pct:
            vals = [ds['jobs'][0].get(direction, {}).get('lat_ns', {}).get('percentile', {}).get(pkey, 0) for ds in datasets]
            pct[pkey] = sum(vals) / count

json.dump(avg, open('$RESULTS_DIR/${prefix}_${name}.json', 'w'), indent=2)
" "$prefix" "$name" "$count"
}

# ─── Generate comparison report ────────────────────────────────────────────

generate_report() {
    local report="$RESULTS_DIR/comparison_report.txt"

    # System info
    local hostname kernel cpu_model
    hostname="$(hostname 2>/dev/null || echo 'unknown')"
    kernel="$(uname -r 2>/dev/null || echo 'unknown')"
    cpu_model="$(grep -m1 'model name' /proc/cpuinfo 2>/dev/null | cut -d: -f2 | xargs || echo 'unknown')"

    {
        echo "=== BFFIO vs FIO Baseline Comparison ==="
        echo "Date: $(date '+%Y-%m-%d %H:%M:%S')"
        echo "Duration: ${DURATION}s per test (mode: $MODE)"
        if [ "$ITERATIONS" -gt 1 ]; then
            echo "Iterations: $ITERATIONS (averaged)"
        fi
        echo "System: $hostname | $kernel | $cpu_model"
        echo ""

        # Table header
        printf "%-18s| %-6s | %-12s | %-12s | %s\n" "Workload" "Metric" "FIO" "BFFIO" "Delta"
        printf "%-18s|--------|--------------|--------------|--------\n" "------------------"

        local iops_wins=0 iops_total=0
        local lat_wins=0 lat_total=0
        local iops_delta_sum=0
        local lat_delta_sum=0
        local regression=0

        for workload in "${WORKLOADS[@]}"; do
            IFS='|' read -r name rw bs depth njobs size extra <<< "$workload"

            local fio_file="$RESULTS_DIR/fio_${name}.json"
            local bf_file="$RESULTS_DIR/BFFIO_${name}.json"

            # Check both files exist
            if [ ! -f "$bf_file" ]; then
                printf "%-18s| %-6s | %-12s | %-12s | %s\n" "$name" "IOPS" "---" "MISSING" "---"
                continue
            fi

            # Extract BFFIO metrics
            local bf_metrics bf_iops bf_bw bf_avg bf_p99
            bf_metrics=$(extract_metrics "$bf_file")
            read -r bf_iops bf_bw bf_avg bf_p99 <<< "$bf_metrics"

            if [ "$SKIP_FIO" -eq 1 ] && [ "$HAS_FIO" -eq 0 ] && [ ! -f "$fio_file" ]; then
                # No FIO data at all -- BFFIO-only report
                printf "%-18s| %-6s | %-12s | %-12s | %s\n" \
                    "$name" "IOPS" "---" "$(fmt_num "$bf_iops")" "---"
                printf "%-18s| %-6s | %-12s | %-12s | %s\n" \
                    "" "BW" "---" "$(fmt_bw "$bf_bw")" "---"
                printf "%-18s| %-6s | %-12s | %-12s | %s\n" \
                    "" "Avg" "---" "$(fmt_lat "$bf_avg")" "---"
                printf "%-18s| %-6s | %-12s | %-12s | %s\n" \
                    "" "P99" "---" "$(fmt_lat "$bf_p99")" "---"
                printf "%-18s|--------|--------------|--------------|--------\n" "------------------"
                continue
            fi

            if [ ! -f "$fio_file" ]; then
                printf "%-18s| %-6s | %-12s | %-12s | %s\n" "$name" "IOPS" "MISSING" "$(fmt_num "$bf_iops")" "---"
                printf "%-18s|--------|--------------|--------------|--------\n" "------------------"
                continue
            fi

            # Extract FIO metrics
            local fio_metrics fio_iops fio_bw fio_avg fio_p99
            fio_metrics=$(extract_metrics "$fio_file")
            read -r fio_iops fio_bw fio_avg fio_p99 <<< "$fio_metrics"

            # Compute deltas
            local d_iops d_bw d_avg d_p99
            d_iops=$(compute_delta "$fio_iops" "$bf_iops")
            d_bw=$(compute_delta "$fio_bw" "$bf_bw")
            d_avg=$(compute_delta "$fio_avg" "$bf_avg")
            d_p99=$(compute_delta "$fio_p99" "$bf_p99")

            # Print rows
            printf "%-18s| %-6s | %12s | %12s | %s\n" \
                "$name" "IOPS" "$(fmt_num "$fio_iops")" "$(fmt_num "$bf_iops")" "$d_iops"
            printf "%-18s| %-6s | %12s | %12s | %s\n" \
                "" "BW" "$(fmt_bw "$fio_bw")" "$(fmt_bw "$bf_bw")" "$d_bw"
            printf "%-18s| %-6s | %12s | %12s | %s\n" \
                "" "Avg" "$(fmt_lat "$fio_avg")" "$(fmt_lat "$bf_avg")" "$d_avg"
            printf "%-18s| %-6s | %12s | %12s | %s\n" \
                "" "P99" "$(fmt_lat "$fio_p99")" "$(fmt_lat "$bf_p99")" "$d_p99"
            printf "%-18s|--------|--------------|--------------|--------\n" "------------------"

            # Track wins/losses for summary
            iops_total=$((iops_total + 1))
            lat_total=$((lat_total + 1))

            # IOPS: higher is better
            local iops_pct
            iops_pct=$(python3 -c "
fv=float('$fio_iops'); bv=float('$bf_iops')
d = ((bv - fv) / fv * 100) if fv > 0 else 0
print(f'{d:.2f}')
")
            iops_delta_sum=$(python3 -c "print(float('$iops_delta_sum') + float('$iops_pct'))")
            if python3 -c "exit(0 if float('$iops_pct') >= 0 else 1)"; then
                iops_wins=$((iops_wins + 1))
            fi

            # Check for regression (BFFIO > 5% slower on IOPS)
            if python3 -c "exit(0 if float('$iops_pct') < -5.0 else 1)"; then
                regression=1
                log "REGRESSION: $name IOPS delta ${iops_pct}%"
            fi

            # Latency: lower is better (negative delta = win)
            local lat_pct
            lat_pct=$(python3 -c "
fv=float('$fio_avg'); bv=float('$bf_avg')
d = ((bv - fv) / fv * 100) if fv > 0 else 0
print(f'{d:.2f}')
")
            lat_delta_sum=$(python3 -c "print(float('$lat_delta_sum') + float('$lat_pct'))")
            if python3 -c "exit(0 if float('$lat_pct') <= 0 else 1)"; then
                lat_wins=$((lat_wins + 1))
            fi
        done

        # Summary
        echo ""
        echo "Summary:"
        if [ "$iops_total" -gt 0 ]; then
            local iops_avg lat_avg
            iops_avg=$(python3 -c "print(f'{float(\"$iops_delta_sum\") / $iops_total:+.1f}')")
            lat_avg=$(python3 -c "print(f'{float(\"$lat_delta_sum\") / $lat_total:+.1f}')")
            echo "  IOPS:    BFFIO wins ${iops_wins}/${iops_total} workloads (avg ${iops_avg}%)"
            echo "  Latency: BFFIO wins ${lat_wins}/${lat_total} workloads (avg ${lat_avg}%)"
        else
            echo "  No comparison data (FIO not available)"
        fi

        if [ "$regression" -eq 1 ]; then
            echo ""
            echo "  WARNING: Regression detected -- BFFIO >5% slower on at least one IOPS workload"
        fi

        # Store regression flag for exit code
        echo "$regression" > "$RESULTS_DIR/.regression_flag"

    } > "$report"

    log "Report written to $report"
}

# ─── Main ───────────────────────────────────────────────────────────────────

main() {
    mkdir -p "$RESULTS_DIR"

    log "BFFIO vs FIO baseline comparison"
    log "Mode: $MODE (${DURATION}s per test), iterations: $ITERATIONS"
    log "Test directory: $TEST_DIR"
    log "Results directory: $RESULTS_DIR"
    echo "" >&2

    local workload_count=${#WORKLOADS[@]}
    local current=0

    for workload in "${WORKLOADS[@]}"; do
        IFS='|' read -r name rw bs depth njobs size extra <<< "$workload"
        current=$((current + 1))

        log "[$current/$workload_count] $name (rw=$rw bs=$bs depth=$depth)"

        for iter in $(seq 0 $((ITERATIONS - 1))); do
            local iter_suffix=""
            if [ "$ITERATIONS" -gt 1 ]; then
                iter_suffix="_iter${iter}"
                log "  Iteration $((iter + 1))/$ITERATIONS"
            fi

            # Parse extra args
            local -a extra_args=()
            if [ -n "${extra:-}" ]; then
                IFS='|' read -ra extra_args <<< "$extra"
            fi

            # Run FIO
            if [ "$SKIP_FIO" -eq 0 ]; then
                prepare_test_dir
                log "  Running FIO..."
                local fio_out="$RESULTS_DIR/fio_${name}${iter_suffix}.json"
                run_fio "$name" "$rw" "$bs" "$depth" "$njobs" "$size" "${extra_args[@]+"${extra_args[@]}"}"
                if [ "$ITERATIONS" -gt 1 ]; then
                    mv "$RESULTS_DIR/fio_${name}.json" "$fio_out"
                fi
            fi

            # Run BFFIO
            prepare_test_dir
            log "  Running BFFIO..."
            local bf_out="$RESULTS_DIR/BFFIO_${name}${iter_suffix}.json"
            run_BFFIO "$name" "$rw" "$bs" "$depth" "$njobs" "$size" "${extra_args[@]+"${extra_args[@]}"}"
            if [ "$ITERATIONS" -gt 1 ]; then
                mv "$RESULTS_DIR/BFFIO_${name}.json" "$bf_out"
            fi
        done

        # Average iterations if needed
        if [ "$ITERATIONS" -gt 1 ]; then
            if [ "$SKIP_FIO" -eq 0 ]; then
                average_iterations "fio" "$name" "$ITERATIONS"
            fi
            average_iterations "BFFIO" "$name" "$ITERATIONS"
        fi
    done

    echo "" >&2
    log "All workloads complete, generating report..."
    generate_report

    # Print report to stdout
    cat "$RESULTS_DIR/comparison_report.txt"

    # Exit code based on regression flag
    local regression
    regression=$(cat "$RESULTS_DIR/.regression_flag" 2>/dev/null || echo "0")
    rm -f "$RESULTS_DIR/.regression_flag"

    if [ "$regression" -eq 1 ]; then
        log "Exit 1: regression detected (BFFIO >5% slower on IOPS)"
        exit 1
    fi

    log "Exit 0: no regressions"
    exit 0
}

main
