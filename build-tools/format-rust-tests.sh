#!/bin/sh
# Format cargo test output into a compact summary table.
# Usage: cargo test 2>&1 | build-tools/format-rust-tests.sh
#
# Input:  raw cargo test output (stdout+stderr merged)
# Output: formatted test lines + summary counts

awk '
/^test / {
    s=$NF; line=$0
    sub(/^test /, "", line); sub(/ \.\.\. [a-zA-Z]+$/, "", line)
    sub(/^tests::/, "", line)
    if (line ~ / - /) {
        path=line; sub(/ - .*/, "", path)
        sub(/^.+\.rs - /, "", line)
        gsub(/\(line [0-9]+\)/, "", line)
        sub(/ *- *compile */, "", line)
        gsub(/^ +| +$/, "", line)
        if (line == "") { n=split(path,a,"/"); line=a[n]; sub(/\.rs$/, "", line) }
    }
    if (s=="ok") { printf "  %-40s OK\n", line; p++ }
    else if (s=="FAILED") { printf "  %-40s FAIL\n", line; f++ }
    else if (s=="ignored") { printf "  %-40s SKIP\n", line; ig++ }
}
END {
    if (f>0) printf "\n%d tests passed, %d FAILED\n",p,f
    else if (ig>0) printf "\n%d tests passed, %d skipped\n",p,ig
    else printf "\n%d tests passed\n",p
}'
