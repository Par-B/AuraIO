#!/usr/bin/env bash
set -euo pipefail

usage() {
    cat <<USAGE
Usage: $0 <output-dir> [--source-root <path>]

Generates API/metrics surface snapshots for freeze and compatibility checks.
USAGE
}

if [[ $# -lt 1 ]]; then
    usage
    exit 1
fi

OUT_DIR=$1
shift

SOURCE_ROOT="$(pwd)"
while [[ $# -gt 0 ]]; do
    case "$1" in
        --source-root)
            SOURCE_ROOT=$2
            shift 2
            ;;
        *)
            echo "Unknown argument: $1" >&2
            usage
            exit 1
            ;;
    esac
done

mkdir -p "$OUT_DIR"

strip_comments() {
    local file=$1
    perl -0777 -pe 's@/\*.*?\*/@@gs' "$file" | sed -E 's@//.*$@@'
}

normalize() {
    sed -E 's/[[:space:]]+/ /g; s/^ //; s/ $//' | awk 'NF > 0'
}

# -----------------------------------------------------------------------------
# C API surface (core public header)
# -----------------------------------------------------------------------------
strip_comments "$SOURCE_ROOT/include/auraio.h" \
    | normalize \
    > "$OUT_DIR/c_api.txt"

# -----------------------------------------------------------------------------
# C++ API surface (declaration-focused extraction from public headers)
# -----------------------------------------------------------------------------
{
    while IFS= read -r file; do
        echo "# file: ${file#$SOURCE_ROOT/}"
        strip_comments "$file" \
            | normalize \
            | awk '
                {
                    line=$0
                    sig=line
                    sub(/\{.*/, "", sig)

                    # Keep major declaration forms and callable signatures.
                    if (sig ~ /^(namespace|class|struct|enum|template|using|typedef) /) {
                        print sig
                        next
                    }
                    if (sig ~ /^(public:|protected:|private:)$/) {
                        print sig
                        next
                    }
                    if (sig ~ /\(/ && sig !~ /^(if|for|while|switch|return)\(/) {
                        print sig
                    }
                }
            '
        echo
    done < <(printf '%s\n' \
        "$SOURCE_ROOT/include/auraio.hpp" \
        "$SOURCE_ROOT/include/auraio"/*.hpp | sort)
} > "$OUT_DIR/cpp_api.txt"

# -----------------------------------------------------------------------------
# Rust API surface (public items from safe crate)
# -----------------------------------------------------------------------------
{
    while IFS= read -r file; do
        echo "# file: ${file#$SOURCE_ROOT/}"
        strip_comments "$file" \
            | normalize \
            | awk '
                {
                    line=$0
                    if (line ~ /^pub\((crate|super|self)\)/) next

                    # Public top-level items and impl methods
                    if (line ~ /^pub (struct|enum|trait|type|fn|mod|use|const) /) {
                        print line
                        next
                    }
                    if (line ~ /^pub unsafe fn / || line ~ /^pub fn /) {
                        print line
                        next
                    }
                }
            '
        echo
    done < <(find "$SOURCE_ROOT/bindings/rust/auraio/src" -maxdepth 1 -type f -name '*.rs' | sort)
} > "$OUT_DIR/rust_api.txt"

# -----------------------------------------------------------------------------
# Prometheus metrics schema surface (metric names only)
# -----------------------------------------------------------------------------
if [[ -f "$SOURCE_ROOT/exporters/prometheus/auraio_prometheus.c" ]]; then
    grep -oE '"auraio_[a-zA-Z0-9_]+' "$SOURCE_ROOT/exporters/prometheus/auraio_prometheus.c" \
        | tr -d '"' \
        | grep -v '^auraio_prometheus$' \
        | sort -u \
        > "$OUT_DIR/prometheus_metrics.txt"
else
    : > "$OUT_DIR/prometheus_metrics.txt"
fi

# Deterministic trailing newline
for f in c_api.txt cpp_api.txt rust_api.txt prometheus_metrics.txt; do
    if [[ -f "$OUT_DIR/$f" ]]; then
        printf '\n' >> "$OUT_DIR/$f"
    fi
done
