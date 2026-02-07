#!/bin/bash
# test_BFFIO.sh - Functional test suite for BFFIO
# Usage: ./test_BFFIO.sh [--quick]
#
# Validates CLI parsing, job file parsing, I/O patterns, JSON output,
# multi-job, multi-thread, ramp time, size-based mode, error handling,
# and AuraIO branding.

set -euo pipefail

# --------------------------------------------------------------------------
# Configuration
# --------------------------------------------------------------------------

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BFFIO="$SCRIPT_DIR/BFFIO"
TEST_DIR="/tmp/BFFIO_test_$$"
PASS=0
FAIL=0

# Default sizes and runtimes (overridden by --quick)
RUNTIME=2
SIZE=64M
SIZE_SMALL=16M
BS_LARGE=64k

if [[ "${1:-}" == "--quick" ]]; then
    RUNTIME=1
    SIZE=16M
    SIZE_SMALL=8M
    BS_LARGE=64k
    echo "Running in quick mode (shorter runtimes, smaller files)"
fi

# --------------------------------------------------------------------------
# Setup and cleanup
# --------------------------------------------------------------------------

cleanup() {
    rm -rf "$TEST_DIR"
}

# Clean up on exit (and on entry for idempotence)
cleanup
trap cleanup EXIT
mkdir -p "$TEST_DIR"

# Verify BFFIO binary exists
if [[ ! -x "$BFFIO" ]]; then
    echo "Error: BFFIO binary not found at $BFFIO"
    echo "Run 'make' in $(dirname "$BFFIO") first."
    exit 1
fi

# --------------------------------------------------------------------------
# Test helper
# --------------------------------------------------------------------------

# test_case NAME EXPECT_FAIL COMMAND...
#   NAME:        test identifier
#   EXPECT_FAIL: "fail" if non-zero exit is expected, "pass" otherwise
#   COMMAND...:  the command to run (remaining arguments)
test_case() {
    local name="$1"
    local expect="$2"
    shift 2

    local rc=0
    # Run the command, capturing output; disable errexit for this invocation
    local output
    output=$("$@" 2>&1) || rc=$?

    if [[ "$expect" == "fail" ]]; then
        if [[ $rc -ne 0 ]]; then
            echo "PASS  $name (expected failure, got exit code $rc)"
            PASS=$((PASS + 1))
        else
            echo "FAIL  $name (expected failure, but command succeeded)"
            echo "  output: ${output:0:200}"
            FAIL=$((FAIL + 1))
        fi
    else
        if [[ $rc -eq 0 ]]; then
            echo "PASS  $name"
            PASS=$((PASS + 1))
        else
            echo "FAIL  $name (exit code $rc)"
            echo "  output: ${output:0:200}"
            FAIL=$((FAIL + 1))
        fi
    fi
}

# --------------------------------------------------------------------------
# 1. Basic I/O patterns (CLI mode)
# --------------------------------------------------------------------------
echo ""
echo "=== Basic I/O Patterns ==="

test_case "randread_4k" pass \
    "$BFFIO" --name=t --rw=randread --bs=4k --size="$SIZE" \
    --directory="$TEST_DIR" --direct=1 --runtime="$RUNTIME" --time_based

test_case "randwrite_4k" pass \
    "$BFFIO" --name=t --rw=randwrite --bs=4k --size="$SIZE" \
    --directory="$TEST_DIR" --direct=1 --runtime="$RUNTIME" --time_based

test_case "seqread_64k" pass \
    "$BFFIO" --name=t --rw=read --bs="$BS_LARGE" --size="$SIZE" \
    --directory="$TEST_DIR" --runtime="$RUNTIME" --time_based

test_case "seqwrite_64k" pass \
    "$BFFIO" --name=t --rw=write --bs="$BS_LARGE" --size="$SIZE" \
    --directory="$TEST_DIR" --runtime="$RUNTIME" --time_based

test_case "mixed_randrw_70" pass \
    "$BFFIO" --name=t --rw=randrw --rwmixread=70 --bs=4k --size="$SIZE" \
    --directory="$TEST_DIR" --direct=1 --runtime="$RUNTIME" --time_based

# --------------------------------------------------------------------------
# 2. JSON output validation
# --------------------------------------------------------------------------
echo ""
echo "=== JSON Output ==="

if command -v jq &>/dev/null; then
    # Run BFFIO with JSON output and validate structure with jq
    json_test() {
        local output
        output=$("$BFFIO" --name=t --rw=randread --bs=4k --size="$SIZE" \
            --directory="$TEST_DIR" --runtime="$RUNTIME" --time_based \
            --output-format=json 2>/dev/null)

        # Verify it's valid JSON and has the expected structure
        echo "$output" | jq -e '.jobs[0].read.iops > 0' >/dev/null 2>&1
    }
    test_case "json_output_valid" pass json_test
else
    echo "SKIP  json_output_valid (jq not installed)"
fi

# --------------------------------------------------------------------------
# 3. Job file parsing
# --------------------------------------------------------------------------
echo ""
echo "=== Job File Parsing ==="

cat > "$TEST_DIR/single.fio" <<EOF
[global]
direct=1
runtime=$RUNTIME
time_based
directory=$TEST_DIR

[readtest]
rw=randread
bs=4k
size=$SIZE
EOF

test_case "job_file_single" pass \
    "$BFFIO" "$TEST_DIR/single.fio"

# --------------------------------------------------------------------------
# 4. Multi-job file
# --------------------------------------------------------------------------
echo ""
echo "=== Multi-Job File ==="

cat > "$TEST_DIR/multi.fio" <<EOF
[global]
runtime=$RUNTIME
time_based
directory=$TEST_DIR

[readjob]
rw=randread
bs=4k
size=$SIZE_SMALL

[writejob]
rw=randwrite
bs=4k
size=$SIZE_SMALL
EOF

test_case "job_file_multi" pass \
    "$BFFIO" "$TEST_DIR/multi.fio"

# --------------------------------------------------------------------------
# 5. Multi-thread (numjobs + group_reporting)
# --------------------------------------------------------------------------
echo ""
echo "=== Multi-Thread ==="

test_case "numjobs_group_reporting" pass \
    "$BFFIO" --name=t --rw=randread --bs=4k --size="$SIZE" \
    --directory="$TEST_DIR" --numjobs=2 --group_reporting \
    --runtime="$RUNTIME" --time_based

# --------------------------------------------------------------------------
# 6. Ramp time
# --------------------------------------------------------------------------
echo ""
echo "=== Ramp Time ==="

test_case "ramp_time" pass \
    "$BFFIO" --name=t --rw=randread --bs=4k --size="$SIZE" \
    --directory="$TEST_DIR" --ramp_time=1 --runtime="$RUNTIME" --time_based

# --------------------------------------------------------------------------
# 7. Size-based mode (no runtime, stop after size bytes)
# --------------------------------------------------------------------------
echo ""
echo "=== Size-Based Mode ==="

test_case "size_based_no_runtime" pass \
    "$BFFIO" --name=t --rw=read --bs="$BS_LARGE" --size="$SIZE_SMALL" \
    --directory="$TEST_DIR"

# --------------------------------------------------------------------------
# 8. Error handling
# --------------------------------------------------------------------------
echo ""
echo "=== Error Handling ==="

# Missing --rw should fail
test_case "missing_rw_fails" fail \
    "$BFFIO" --name=t --bs=4k --size="$SIZE" --directory="$TEST_DIR"

# Nonexistent .fio file should fail
test_case "nonexistent_fio_fails" fail \
    "$BFFIO" "$TEST_DIR/does_not_exist.fio"

# --------------------------------------------------------------------------
# 9. Target P99 latency mode
# --------------------------------------------------------------------------
echo ""
echo "=== Target P99 Latency ==="

target_p99_test() {
    "$BFFIO" --name=t --rw=randread --bs=4k --size="$SIZE" \
        --directory="$TEST_DIR" --direct=1 --runtime="$RUNTIME" --time_based \
        --target-p99=5ms 2>&1 \
        | grep -q 'target: 5.00ms'
}
test_case "target_p99_ms_output" pass target_p99_test

target_p99_us_test() {
    "$BFFIO" --name=t --rw=randread --bs=4k --size="$SIZE" \
        --directory="$TEST_DIR" --direct=1 --runtime="$RUNTIME" --time_based \
        --target-p99=500us 2>&1 \
        | grep -q 'target: 0.50ms'
}
test_case "target_p99_usec_suffix" pass target_p99_us_test

target_p99_concurrency_test() {
    "$BFFIO" --name=t --rw=randread --bs=4k --size="$SIZE" \
        --directory="$TEST_DIR" --direct=1 --runtime="$RUNTIME" --time_based \
        --target-p99=5ms 2>&1 \
        | grep -q 'max concurrency='
}
test_case "target_p99_max_concurrency" pass target_p99_concurrency_test

if command -v jq &>/dev/null; then
    target_p99_json_test() {
        local output
        output=$("$BFFIO" --name=t --rw=randread --bs=4k --size="$SIZE" \
            --directory="$TEST_DIR" --direct=1 --runtime="$RUNTIME" --time_based \
            --target-p99=2ms --output-format=json 2>/dev/null)
        echo "$output" | jq -e '.jobs[0].AuraIO.target_p99_ms == 2' >/dev/null 2>&1 && \
        echo "$output" | jq -e '.jobs[0].AuraIO.max_concurrency > 0' >/dev/null 2>&1
    }
    test_case "target_p99_json_fields" pass target_p99_json_test
else
    echo "SKIP  target_p99_json_fields (jq not installed)"
fi

# --------------------------------------------------------------------------
# 10. AuraIO branding in output
# --------------------------------------------------------------------------
echo ""
echo "=== AuraIO Branding ==="

branding_test() {
    "$BFFIO" --name=t --rw=randread --bs=4k --size="$SIZE_SMALL" \
        --directory="$TEST_DIR" --runtime="$RUNTIME" --time_based 2>&1 \
        | grep -q 'AuraIO'
}
test_case "output_contains_auraio" pass branding_test

# --------------------------------------------------------------------------
# Summary
# --------------------------------------------------------------------------
echo ""
echo "==============================="
echo "$PASS passed, $FAIL failed"
echo "==============================="

if [[ $FAIL -gt 0 ]]; then
    exit 1
fi
exit 0
