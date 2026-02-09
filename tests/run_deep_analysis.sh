#!/bin/bash
#
# AuraIO Deep Performance Analysis
# Systematic profiling of hot paths to identify optimization opportunities
#
# Usage: ./run_deep_analysis.sh [options]
#   --quick          5s perf, 1s valgrind (~10 min)
#   --standard       10s perf, 2s valgrind (~20 min, default)
#   --full           20s perf, 3s valgrind (~40 min)
#   --skip-valgrind  Skip DRD/cachegrind/callgrind phases
#   --skip-install   Don't install pahole/FlameGraph
#   --help           Show this help
#
# Requires: perf, valgrind, pahole (dwarves), FlameGraph scripts
# Run 'make deps' first if missing dependencies.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(dirname "$SCRIPT_DIR")"
RESULTS_DIR="${SCRIPT_DIR}/bench_results/deep"
REPORT_FILE="${RESULTS_DIR}/deep_analysis_report.txt"
TEST_DIR="/tmp/auraio_bench"
FLAMEGRAPH_DIR="/tmp/FlameGraph"

# Defaults
PERF_DURATION=10
VG_DURATION=2
RUN_VALGRIND=true
DO_INSTALL=true
LABEL="standard"

while [[ $# -gt 0 ]]; do
    case $1 in
        --quick)          PERF_DURATION=5;  VG_DURATION=1; LABEL="quick"; shift ;;
        --standard)       PERF_DURATION=10; VG_DURATION=2; LABEL="standard"; shift ;;
        --full)           PERF_DURATION=20; VG_DURATION=3; LABEL="full"; shift ;;
        --skip-valgrind)  RUN_VALGRIND=false; shift ;;
        --skip-install)   DO_INSTALL=false; shift ;;
        --dir)            TEST_DIR="$2"; shift 2 ;;
        --help|-h)
            head -n 15 "$0" | tail -n 13
            exit 0 ;;
        *) echo "Unknown option: $1"; exit 1 ;;
    esac
done

export AURAIO_BENCH_DIR="$TEST_DIR"

# Colors
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
CYAN='\033[0;36m'
RED='\033[0;31m'
NC='\033[0m'

mkdir -p "$RESULTS_DIR"
mkdir -p "$TEST_DIR"

cleanup() { rm -rf "$TEST_DIR"; }
trap cleanup EXIT

info()  { echo -e "${CYAN}$*${NC}"; }
warn()  { echo -e "${YELLOW}$*${NC}"; }
ok()    { echo -e "${GREEN}$*${NC}"; }
fail()  { echo -e "${RED}$*${NC}"; }

# ============================================================================
# Phase 0: Prerequisites
# ============================================================================
phase_prerequisites() {
    info "=== Phase 0: Prerequisites ==="
    echo ""

    # Check perf
    if ! command -v perf &>/dev/null; then
        fail "  perf not found. Install with: sudo apt install linux-tools-common linux-tools-$(uname -r)"
        exit 1
    fi
    ok "  [OK] perf: $(perf version 2>&1 | head -1)"

    # Check perf permissions
    local paranoid
    paranoid=$(cat /proc/sys/kernel/perf_event_paranoid 2>/dev/null || echo "4")
    if [ "$paranoid" -le 2 ] || [ "$(id -u)" -eq 0 ]; then
        ok "  [OK] perf_event_paranoid=$paranoid"
    else
        warn "  [WARN] perf_event_paranoid=$paranoid (some events may be restricted)"
    fi

    # Check valgrind
    if [ "$RUN_VALGRIND" = true ]; then
        if command -v valgrind &>/dev/null; then
            ok "  [OK] valgrind: $(valgrind --version 2>&1)"
        else
            warn "  [WARN] valgrind not found, skipping valgrind phases"
            RUN_VALGRIND=false
        fi
    else
        echo "  [SKIP] valgrind (--skip-valgrind)"
    fi

    # Check/install pahole
    if command -v pahole &>/dev/null; then
        ok "  [OK] pahole: $(pahole --version 2>&1 | head -1)"
    elif [ "$DO_INSTALL" = true ]; then
        info "  Installing dwarves (for pahole)..."
        if command -v sudo &>/dev/null; then
            sudo apt-get install -y dwarves > /dev/null 2>&1 && \
                ok "  [OK] pahole installed" || \
                warn "  [WARN] Failed to install dwarves, skipping struct analysis"
        else
            warn "  [WARN] No sudo, cannot install dwarves"
        fi
    else
        warn "  [WARN] pahole not found (install dwarves package)"
    fi

    # Check/install FlameGraph
    if [ -d "$FLAMEGRAPH_DIR" ] && [ -f "$FLAMEGRAPH_DIR/flamegraph.pl" ]; then
        ok "  [OK] FlameGraph scripts: $FLAMEGRAPH_DIR"
    elif [ "$DO_INSTALL" = true ]; then
        info "  Cloning FlameGraph scripts..."
        git clone --depth 1 https://github.com/brendangregg/FlameGraph.git "$FLAMEGRAPH_DIR" \
            > /dev/null 2>&1 && \
            ok "  [OK] FlameGraph cloned to $FLAMEGRAPH_DIR" || \
            warn "  [WARN] Failed to clone FlameGraph, skipping flamegraph generation"
    else
        warn "  [WARN] FlameGraph not found at $FLAMEGRAPH_DIR"
    fi

    echo ""
}

# ============================================================================
# Debug build with frame pointers
# ============================================================================
build_debug() {
    info "=== Building with debug symbols + frame pointers ==="

    cd "$PROJECT_ROOT"

    # Clean and rebuild with -g -fno-omit-frame-pointer for accurate profiling
    # Keep -O2 for realistic optimization (not -O0 which distorts profiles)
    # Pass CFLAGS as make argument to override Makefile's assignment
    local DEBUG_CFLAGS="-Wall -Wextra -std=c11 -O2 -fPIC -Icore/include -Icore/src -g -fno-omit-frame-pointer"
    make clean > /dev/null 2>&1
    make -j"$(nproc)" all CFLAGS="$DEBUG_CFLAGS" > /dev/null 2>&1

    # Rebuild perf_bench with same flags
    local BENCH_CFLAGS="-Wall -Wextra -std=c11 -O2 -g -fno-omit-frame-pointer -I../core/include -I../core/src"
    make -C tests perf_bench CFLAGS="$BENCH_CFLAGS" > /dev/null 2>&1

    ok "  Build complete (-O2 -g -fno-omit-frame-pointer)"
    cd "$SCRIPT_DIR"
    echo ""
}

# ============================================================================
# Phase 1: Flamegraphs
# ============================================================================
phase_flamegraphs() {
    info "=== Phase 1: CPU Flamegraphs (perf record, ${PERF_DURATION}s each) ==="

    if [ ! -d "$FLAMEGRAPH_DIR" ] || [ ! -f "$FLAMEGRAPH_DIR/flamegraph.pl" ]; then
        warn "  FlameGraph scripts not available, skipping flamegraphs"
        return
    fi

    local bench="$SCRIPT_DIR/perf_bench"

    for workload in throughput latency buffer; do
        echo -n "  $workload ... "
        local data_file="$RESULTS_DIR/perf_${workload}.data"

        perf record -e cpu-clock -g --call-graph dwarf \
            -o "$data_file" \
            -- "$bench" "$workload" "$PERF_DURATION" > /dev/null 2>&1

        # Generate collapsed stacks (save for report parsing)
        perf script -i "$data_file" 2>/dev/null | \
            "$FLAMEGRAPH_DIR/stackcollapse-perf.pl" \
            > "$RESULTS_DIR/collapsed_${workload}.txt" 2>/dev/null

        # Generate SVG
        "$FLAMEGRAPH_DIR/flamegraph.pl" \
            --title "AuraIO $workload" \
            < "$RESULTS_DIR/collapsed_${workload}.txt" \
            > "$RESULTS_DIR/flamegraph_${workload}.svg" 2>/dev/null

        # Clean up the large perf.data file
        rm -f "$data_file"

        ok "done"
    done
    echo ""
}

# ============================================================================
# Phase 2: Lock contention
# ============================================================================
phase_lock_contention() {
    info "=== Phase 2: Lock Contention Analysis ==="

    local bench="$SCRIPT_DIR/perf_bench"

    # perf stat with context-switches as contention proxy
    for workload in throughput latency buffer; do
        echo -n "  perf stat $workload ... "
        perf stat -e task-clock,context-switches,cpu-migrations,page-faults \
            "$bench" "$workload" "$PERF_DURATION" \
            > "$RESULTS_DIR/perf_stat_${workload}.txt" 2>&1
        ok "done"
    done

    # Try perf lock contention (may not work without tracepoints/BPF)
    echo -n "  perf lock contention ... "
    if perf lock contention -E 20 -- "$bench" throughput 3 \
            > "$RESULTS_DIR/perf_lock_throughput.txt" 2>&1; then
        ok "done"
    else
        warn "skipped (tracepoints unavailable)"
        echo "perf lock contention not available" > "$RESULTS_DIR/perf_lock_throughput.txt"
    fi

    # Valgrind DRD for detailed mutex analysis
    if [ "$RUN_VALGRIND" = true ]; then
        for workload in latency throughput; do
            echo -n "  DRD $workload ... "
            valgrind --tool=drd \
                --report-signal-unlocked=no \
                --check-stack-var=no \
                --exclusive-threshold=1 \
                "$bench" "$workload" "$VG_DURATION" \
                > "$RESULTS_DIR/drd_${workload}.txt" 2>&1 || true
            ok "done"
        done
    fi

    echo ""
}

# ============================================================================
# Phase 3: Cache behavior
# ============================================================================
phase_cache_behavior() {
    info "=== Phase 3: Cache Behavior Analysis ==="

    local bench="$SCRIPT_DIR/perf_bench"

    # Try hardware cache counters (best-effort in VM)
    echo -n "  perf stat cache counters ... "
    perf stat -e task-clock,L1-dcache-loads,L1-dcache-load-misses,L1-dcache-stores,LLC-loads,LLC-load-misses \
        "$bench" throughput "$PERF_DURATION" \
        > "$RESULTS_DIR/perf_stat_cache.txt" 2>&1 || true
    ok "done"

    # Cachegrind (simulated cache, works in VM)
    if [ "$RUN_VALGRIND" = true ]; then
        for workload in throughput buffer; do
            echo -n "  cachegrind $workload ... "
            valgrind --tool=cachegrind \
                --cachegrind-out-file="$RESULTS_DIR/cachegrind_${workload}.out" \
                "$bench" "$workload" "$VG_DURATION" \
                > "$RESULTS_DIR/cachegrind_${workload}_summary.txt" 2>&1 || true

            # Annotate source files
            if [ -f "$RESULTS_DIR/cachegrind_${workload}.out" ]; then
                cg_annotate --auto=yes "$RESULTS_DIR/cachegrind_${workload}.out" \
                    > "$RESULTS_DIR/cachegrind_${workload}_annotated.txt" 2>&1 || true
            fi
            ok "done"
        done
    fi

    echo ""
}

# ============================================================================
# Phase 4: Struct layout
# ============================================================================
phase_struct_layout() {
    info "=== Phase 4: Struct Layout Analysis (pahole) ==="

    if ! command -v pahole &>/dev/null; then
        warn "  pahole not available, skipping struct analysis"
        return
    fi

    local lib="$PROJECT_ROOT/core/lib/libauraio.a"
    if [ ! -f "$lib" ]; then
        warn "  $lib not found"
        return
    fi

    # Extract a .o file for pahole (it needs ELF with DWARF, .a works too)
    local structs=(
        "ring_ctx"
        "auraio_request"
        "adaptive_controller_t"
        "adaptive_histogram_t"
        "adaptive_histogram_pair_t"
        "buffer_pool"
        "buffer_shard_t"
        "thread_cache"
        "auraio_engine"
    )

    for s in "${structs[@]}"; do
        echo -n "  $s ... "
        local outfile="$RESULTS_DIR/pahole_${s}.txt"
        # pahole searches for struct/class names in DWARF info
        pahole --class_name="$s" "$lib" > "$outfile" 2>&1 || true
        if [ -s "$outfile" ]; then
            ok "done"
        else
            warn "not found"
        fi
    done

    # Overall holes report
    echo -n "  all structs with holes ... "
    pahole --holes 1 "$lib" > "$RESULTS_DIR/pahole_all_holes.txt" 2>&1 || true
    ok "done"

    echo ""
}

# ============================================================================
# Phase 5: Callgraph
# ============================================================================
phase_callgraph() {
    info "=== Phase 5: Callgraph Analysis (callgrind) ==="

    if [ "$RUN_VALGRIND" != true ]; then
        warn "  Skipped (--skip-valgrind)"
        return
    fi

    local bench="$SCRIPT_DIR/perf_bench"

    for workload in latency throughput; do
        echo -n "  callgrind $workload ... "
        valgrind --tool=callgrind \
            --callgrind-out-file="$RESULTS_DIR/callgrind_${workload}.out" \
            --collect-jumps=yes \
            "$bench" "$workload" "$VG_DURATION" \
            > "$RESULTS_DIR/callgrind_${workload}_summary.txt" 2>&1 || true

        if [ -f "$RESULTS_DIR/callgrind_${workload}.out" ]; then
            callgrind_annotate --auto=yes --inclusive=yes \
                "$RESULTS_DIR/callgrind_${workload}.out" \
                > "$RESULTS_DIR/callgrind_${workload}_annotated.txt" 2>&1 || true
        fi
        ok "done"
    done

    echo ""
}

# ============================================================================
# Phase 6: False sharing detection (perf c2c)
# ============================================================================
phase_false_sharing() {
    info "=== Phase 6: False Sharing Detection (perf c2c, best-effort) ==="

    local bench="$SCRIPT_DIR/perf_bench"

    for workload in throughput buffer; do
        echo -n "  perf c2c $workload ... "
        if perf c2c record -g -o "$RESULTS_DIR/perf_c2c_${workload}.data" \
                -- "$bench" "$workload" "$PERF_DURATION" > /dev/null 2>&1; then
            perf c2c report -i "$RESULTS_DIR/perf_c2c_${workload}.data" \
                --stdio > "$RESULTS_DIR/perf_c2c_${workload}_report.txt" 2>&1 || true
            rm -f "$RESULTS_DIR/perf_c2c_${workload}.data"
            ok "done"
        else
            warn "not available in VM"
            echo "perf c2c not available (hardware counters required)" \
                > "$RESULTS_DIR/perf_c2c_${workload}_report.txt"
        fi
    done

    echo ""
}

# ============================================================================
# Phase 7: Report generation
# ============================================================================
generate_report() {
    info "=== Phase 7: Generating Deep Analysis Report ==="

    python3 - "$RESULTS_DIR" "$PERF_DURATION" "$VG_DURATION" "$LABEL" "$RUN_VALGRIND" << 'PYEOF'
import re, sys, os, glob

results_dir = sys.argv[1]
perf_dur = sys.argv[2]
vg_dur = sys.argv[3]
label = sys.argv[4]
ran_valgrind = sys.argv[5] == "true"

report = []
R = report.append

R("=" * 72)
R("AuraIO Deep Performance Analysis Report")
R("=" * 72)
R(f"Profile: {label} (perf={perf_dur}s, valgrind={vg_dur}s)")
R("")

# -----------------------------------------------------------------------
# Section 1: CPU Time Distribution (from flamegraph collapsed stacks)
# -----------------------------------------------------------------------
R("-" * 72)
R("1. CPU TIME DISTRIBUTION")
R("-" * 72)

for workload in ["throughput", "latency", "buffer"]:
    collapsed_file = os.path.join(results_dir, f"collapsed_{workload}.txt")
    if not os.path.exists(collapsed_file):
        continue

    lines = open(collapsed_file).readlines()
    total_samples = sum(int(l.strip().split()[-1]) for l in lines if l.strip())
    if total_samples == 0:
        continue

    # Categorize by searching the FULL stack (deepest library-level match wins)
    # Order matters: more specific patterns checked first
    categories = {
        'clock_gettime':      0,
        'sched_getcpu':       0,
        'pthread_mutex':      0,
        'adaptive_hist':      0,
        'adaptive_tick':      0,
        'buffer_pool':        0,
        'ring_submit':        0,
        'ring_poll/complete':  0,
        'io_uring kernel':    0,
        'auraio (other)':     0,
        'malloc/free':        0,
        'benchmark overhead': 0,
        'rand()':             0,
        'kernel/unknown':     0,
    }

    # Rules: (search_in, pattern, category) — checked in order, first match wins
    # search_in: 'stack' = full stack string, 'leaf' = leaf function only
    rules = [
        # Specific library subsystems (search full stack)
        ('stack', 'get_time_ns',          'clock_gettime'),
        ('stack', 'clock_gettime',        'clock_gettime'),
        ('stack', '__vdso_clock',         'clock_gettime'),
        ('stack', 'sched_getcpu',         'sched_getcpu'),
        ('stack', '__vdso_getcpu',        'sched_getcpu'),
        ('stack', 'adaptive_hist',        'adaptive_hist'),
        ('stack', 'hist_record',          'adaptive_hist'),
        ('stack', 'adaptive_tick',        'adaptive_tick'),
        ('stack', 'buffer_pool',          'buffer_pool'),
        ('stack', 'get_thread_cache',     'buffer_pool'),
        ('stack', 'process_completion',   'ring_poll/complete'),
        ('stack', 'ring_poll',            'ring_poll/complete'),
        ('stack', 'ring_flush',           'ring_poll/complete'),
        ('stack', 'ring_wait',            'ring_poll/complete'),
        ('stack', 'ring_submit',          'ring_submit'),
        ('stack', 'submit_begin',         'ring_submit'),
        ('stack', 'submit_end',           'ring_submit'),
        ('stack', 'auraio_read',          'ring_submit'),
        ('stack', 'auraio_write',         'ring_submit'),
        # Mutex (leaf only - attribute to mutex itself, not caller)
        ('leaf',  'pthread_mutex',        'pthread_mutex'),
        ('leaf',  '__lll_lock',           'pthread_mutex'),
        ('leaf',  'futex',                'pthread_mutex'),
        # io_uring kernel path
        ('stack', 'io_uring',             'io_uring kernel'),
        # Other library code
        ('stack', 'auraio_',              'auraio (other)'),
        ('stack', 'ring_',                'auraio (other)'),
        ('stack', 'adaptive_',            'auraio (other)'),
        # malloc/free
        ('stack', 'malloc',               'malloc/free'),
        ('stack', '_int_malloc',          'malloc/free'),
        ('stack', 'free',                 'malloc/free'),
        ('stack', 'cfree',                'malloc/free'),
        # Benchmark random number generation
        ('stack', 'random_r',             'rand()'),
        ('stack', 'random',               'rand()'),
        ('leaf',  'rand',                 'rand()'),
        # Benchmark code
        ('stack', 'bench_',               'benchmark overhead'),
        ('stack', 'read_callback',        'benchmark overhead'),
    ]

    for line in lines:
        parts = line.strip().rsplit(None, 1)
        if len(parts) != 2:
            continue
        stack, count = parts[0], int(parts[1])
        leaf = stack.split(';')[-1] if ';' in stack else stack

        matched = False
        for search_in, pattern, category in rules:
            target = stack if search_in == 'stack' else leaf
            if pattern in target:
                categories[category] += count
                matched = True
                break

        if not matched:
            categories['kernel/unknown'] += count

    R(f"\n  {workload.upper()} ({total_samples:,} samples):")
    R(f"    {'Category':<25} {'Samples':>12} {'%':>7}")
    R(f"    {'-'*25} {'-'*12} {'-'*7}")
    for cat, cnt in sorted(categories.items(), key=lambda x: -x[1]):
        if cnt > 0:
            pct = cnt / total_samples * 100
            R(f"    {cat:<25} {cnt:>12,} {pct:>6.1f}%")

R("")

# -----------------------------------------------------------------------
# Section 2: Lock Contention
# -----------------------------------------------------------------------
R("-" * 72)
R("2. LOCK CONTENTION")
R("-" * 72)

for workload in ["throughput", "latency", "buffer"]:
    stat_file = os.path.join(results_dir, f"perf_stat_{workload}.txt")
    if not os.path.exists(stat_file):
        continue

    text = open(stat_file).read()
    R(f"\n  {workload.upper()}:")

    # Parse perf stat output
    cs_match = re.search(r'([\d,]+)\s+context-switches', text)
    mig_match = re.search(r'([\d,]+)\s+cpu-migrations', text)
    pf_match = re.search(r'([\d,]+)\s+page-faults', text)
    tc_match = re.search(r'([\d,.]+)\s+msec\s+task-clock', text)
    time_match = re.search(r'([\d.]+)\s+seconds time elapsed', text)

    if cs_match:
        cs = int(cs_match.group(1).replace(',', ''))
        elapsed = float(time_match.group(1)) if time_match else 1
        R(f"    Context switches:  {cs:>10,}  ({cs/elapsed:,.0f}/sec)")
    if mig_match:
        R(f"    CPU migrations:    {int(mig_match.group(1).replace(',','')):>10,}")
    if pf_match:
        R(f"    Page faults:       {int(pf_match.group(1).replace(',','')):>10,}")
    if tc_match:
        R(f"    Task clock:        {tc_match.group(1):>10} msec")

# perf lock contention
lock_file = os.path.join(results_dir, "perf_lock_throughput.txt")
if os.path.exists(lock_file):
    text = open(lock_file).read()
    if 'not available' not in text and len(text.strip()) > 50:
        R(f"\n  MUTEX CONTENTION (perf lock):")
        for line in text.strip().split('\n')[:20]:
            R(f"    {line}")

R("")

# -----------------------------------------------------------------------
# Section 3: Cache Efficiency
# -----------------------------------------------------------------------
R("-" * 72)
R("3. CACHE EFFICIENCY")
R("-" * 72)

# Hardware counters (best-effort)
cache_file = os.path.join(results_dir, "perf_stat_cache.txt")
if os.path.exists(cache_file):
    text = open(cache_file).read()
    R(f"\n  Hardware cache counters (may be unavailable in VM):")
    for line in text.strip().split('\n'):
        line = line.strip()
        if any(k in line for k in ['L1-dcache', 'LLC-load', 'not supported', 'not counted']):
            R(f"    {line}")

# Cachegrind summaries
for workload in ["throughput", "buffer"]:
    summary_file = os.path.join(results_dir, f"cachegrind_{workload}_summary.txt")
    if not os.path.exists(summary_file):
        continue

    text = open(summary_file).read()
    R(f"\n  Cachegrind {workload.upper()} summary:")

    # Extract the summary line from cachegrind output
    for line in text.split('\n'):
        if 'I   refs' in line or 'D   refs' in line or 'LL refs' in line or \
           'I1  miss' in line or 'D1  miss' in line or 'LL miss' in line or \
           'Branches' in line:
            R(f"    {line.strip()}")

# Top cachegrind hotspots
for workload in ["throughput", "buffer"]:
    ann_file = os.path.join(results_dir, f"cachegrind_{workload}_annotated.txt")
    if not os.path.exists(ann_file):
        continue

    text = open(ann_file).read()
    R(f"\n  Cachegrind {workload.upper()} top functions (by D1 miss rate):")

    # Find the per-function summary table in cg_annotate output
    in_summary = False
    func_lines = []
    for line in text.split('\n'):
        if 'file:function' in line.lower():
            in_summary = True
            continue
        if in_summary:
            if line.strip() == '' or line.startswith('-'):
                if func_lines:
                    break
                continue
            func_lines.append(line)

    for line in func_lines[:15]:
        R(f"    {line.rstrip()}")

R("")

# -----------------------------------------------------------------------
# Section 4: Struct Layout
# -----------------------------------------------------------------------
R("-" * 72)
R("4. STRUCT LAYOUT (pahole)")
R("-" * 72)

struct_files = sorted(glob.glob(os.path.join(results_dir, "pahole_*.txt")))
for sf in struct_files:
    name = os.path.basename(sf).replace("pahole_", "").replace(".txt", "")
    if name == "all_holes":
        continue
    text = open(sf).read().strip()
    if not text or 'not found' in text:
        continue

    R(f"\n  {name}:")

    # Show the pahole output (struct layout with offsets and sizes)
    for line in text.split('\n'):
        R(f"    {line}")

# Holes summary
holes_file = os.path.join(results_dir, "pahole_all_holes.txt")
if os.path.exists(holes_file):
    text = open(holes_file).read().strip()
    if text:
        R(f"\n  STRUCTS WITH PADDING HOLES:")
        shown = 0
        for line in text.split('\n'):
            if shown > 30:
                R("    ... (truncated)")
                break
            R(f"    {line}")
            shown += 1

R("")

# -----------------------------------------------------------------------
# Section 5: Callgraph (function call counts)
# -----------------------------------------------------------------------
R("-" * 72)
R("5. CALLGRAPH ANALYSIS")
R("-" * 72)

for workload in ["latency", "throughput"]:
    ann_file = os.path.join(results_dir, f"callgrind_{workload}_annotated.txt")
    if not os.path.exists(ann_file):
        continue

    text = open(ann_file).read()
    R(f"\n  {workload.upper()} top functions (inclusive instruction cost):")

    # Find the function summary in callgrind_annotate output
    in_summary = False
    func_lines = []
    for line in text.split('\n'):
        if 'file:function' in line.lower():
            in_summary = True
            continue
        if in_summary:
            if line.strip() == '' or line.startswith('-'):
                if func_lines:
                    break
                continue
            func_lines.append(line)

    for line in func_lines[:20]:
        R(f"    {line.rstrip()}")

R("")

# -----------------------------------------------------------------------
# Section 6: False Sharing (perf c2c)
# -----------------------------------------------------------------------
R("-" * 72)
R("6. FALSE SHARING (perf c2c)")
R("-" * 72)

for workload in ["throughput", "buffer"]:
    c2c_file = os.path.join(results_dir, f"perf_c2c_{workload}_report.txt")
    if not os.path.exists(c2c_file):
        continue

    text = open(c2c_file).read()
    if 'not available' in text:
        R(f"\n  {workload}: perf c2c not available (hardware counters required)")
    else:
        R(f"\n  {workload.upper()} cache-to-cache transfers:")
        # Show the top shared cache lines
        shown = 0
        for line in text.split('\n'):
            if shown > 30:
                R("    ... (truncated)")
                break
            R(f"    {line}")
            shown += 1

R("")

# -----------------------------------------------------------------------
# Section 7: Optimization Recommendations
# -----------------------------------------------------------------------
R("-" * 72)
R("7. OPTIMIZATION RECOMMENDATIONS")
R("-" * 72)
R("")
R("  Based on profiling data above. Review flamegraphs and annotated")
R("  source for detailed analysis.")
R("")

recommendations = []

# Check if sched_getcpu is visible in flamegraphs
for workload in ["throughput", "latency"]:
    collapsed_file = os.path.join(results_dir, f"collapsed_{workload}.txt")
    if not os.path.exists(collapsed_file):
        continue
    lines = open(collapsed_file).readlines()
    total = sum(int(l.strip().split()[-1]) for l in lines if l.strip())
    getcpu = sum(int(l.strip().split()[-1]) for l in lines
                 if l.strip() and ('sched_getcpu' in l or 'getcpu' in l or '__vdso_getcpu' in l))
    if total > 0 and getcpu / total > 0.005:
        pct = getcpu / total * 100
        recommendations.append(
            f"[HIGH] sched_getcpu() overhead: {pct:.1f}% of {workload} CPU time\n"
            f"         -> Cache CPU index in TLS, refresh every ~1000 ops or per tick (10ms)")
        break

# Check clock_gettime overhead
for workload in ["throughput", "latency"]:
    collapsed_file = os.path.join(results_dir, f"collapsed_{workload}.txt")
    if not os.path.exists(collapsed_file):
        continue
    lines = open(collapsed_file).readlines()
    total = sum(int(l.strip().split()[-1]) for l in lines if l.strip())
    clk = sum(int(l.strip().split()[-1]) for l in lines
              if l.strip() and ('clock_gettime' in l or '__vdso_clock' in l or 'get_time_ns' in l))
    if total > 0 and clk / total > 0.01:
        pct = clk / total * 100
        recommendations.append(
            f"[HIGH] clock_gettime() overhead: {pct:.1f}% of {workload} CPU time\n"
            f"         -> Consider CLOCK_MONOTONIC_COARSE (1-4ms resolution)\n"
            f"         -> Or reduce to 1 call per I/O (completion only, not submit)")
        break

# Check mutex overhead
for workload in ["throughput"]:
    collapsed_file = os.path.join(results_dir, f"collapsed_{workload}.txt")
    if not os.path.exists(collapsed_file):
        continue
    lines = open(collapsed_file).readlines()
    total = sum(int(l.strip().split()[-1]) for l in lines if l.strip())
    mtx = sum(int(l.strip().split()[-1]) for l in lines
              if l.strip() and ('pthread_mutex' in l or '__lll_lock' in l or 'futex' in l))
    if total > 0 and mtx / total > 0.05:
        pct = mtx / total * 100
        recommendations.append(
            f"[HIGH] Mutex overhead: {pct:.1f}% of {workload} CPU time\n"
            f"         -> process_completion() does 4 mutex ops per I/O completion\n"
            f"         -> Batch counter updates + request return in single critical section")
        break

# Check malloc/free overhead (benchmark artifact vs library)
for workload in ["throughput", "latency"]:
    collapsed_file = os.path.join(results_dir, f"collapsed_{workload}.txt")
    if not os.path.exists(collapsed_file):
        continue
    lines = open(collapsed_file).readlines()
    total = sum(int(l.strip().split()[-1]) for l in lines if l.strip())
    alloc = sum(int(l.strip().split()[-1]) for l in lines
                if l.strip() and ('malloc' in l or 'free' in l or 'cfree' in l or 'tcache' in l))
    if total > 0 and alloc / total > 0.03:
        pct = alloc / total * 100
        recommendations.append(
            f"[MED]  malloc/free overhead: {pct:.1f}% of {workload} CPU time\n"
            f"         -> Likely from per-op request_ctx_t allocation in benchmark\n"
            f"         -> Consider pooling in benchmark, or adding a request pool API")
        break

# Check context switches
for workload in ["throughput"]:
    stat_file = os.path.join(results_dir, f"perf_stat_{workload}.txt")
    if not os.path.exists(stat_file):
        continue
    text = open(stat_file).read()
    cs_match = re.search(r'([\d,]+)\s+context-switches', text)
    time_match = re.search(r'([\d.]+)\s+seconds time elapsed', text)
    if cs_match and time_match:
        cs = int(cs_match.group(1).replace(',', ''))
        elapsed = float(time_match.group(1))
        cs_per_sec = cs / elapsed
        if cs_per_sec > 5000:
            recommendations.append(
                f"[MED]  High context switches: {cs_per_sec:,.0f}/sec during {workload}\n"
                f"         -> Indicates mutex contention causing thread yielding\n"
                f"         -> Consider spinlock for short critical sections or lock-free paths")

# Always recommend struct alignment check
recommendations.append(
    f"[LOW]  Review pahole output for false sharing\n"
    f"         -> Check if atomic fields in adaptive_controller_t share cache lines\n"
    f"           with fields modified under mutex in ring_ctx_t\n"
    f"         -> Add __attribute__((aligned(64))) padding if needed")

for i, rec in enumerate(recommendations, 1):
    R(f"  {i}. {rec}")
    R("")

R("=" * 72)
R("End of report. Review flamegraph SVGs for visual analysis.")
R("=" * 72)

output = '\n'.join(report)
print(output)

report_file = os.path.join(results_dir, "deep_analysis_report.txt")
with open(report_file, 'w') as f:
    f.write(output + '\n')

print(f"\nReport saved to: {report_file}")
PYEOF
}

# ============================================================================
# Main
# ============================================================================

echo "============================================"
echo "AuraIO Deep Performance Analysis"
echo "Profile: $LABEL (perf=${PERF_DURATION}s, valgrind=${VG_DURATION}s)"
echo "============================================"
echo ""

phase_prerequisites
build_debug
phase_flamegraphs
phase_lock_contention
phase_cache_behavior
phase_struct_layout
phase_callgraph
phase_false_sharing
generate_report

echo ""
ok "Deep analysis complete!"
echo "  Results: $RESULTS_DIR/"
echo "  Report:  $REPORT_FILE"
echo ""
echo "  Flamegraphs (open in browser):"
for svg in "$RESULTS_DIR"/flamegraph_*.svg; do
    [ -f "$svg" ] && echo "    $svg"
done
