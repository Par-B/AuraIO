#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
BASE_REF="${1:-${BASE_REF:-origin/main}}"

if [[ "${ENFORCE_API_FREEZE:-0}" != "1" ]]; then
    echo "API freeze enforcement is disabled (ENFORCE_API_FREEZE!=1)."
    exit 0
fi

if ! git -C "$ROOT" rev-parse --verify "$BASE_REF" >/dev/null 2>&1; then
    echo "Base ref '$BASE_REF' not found; skipping RFC gate." >&2
    exit 0
fi

changed="$(git -C "$ROOT" diff --name-only "$BASE_REF"...HEAD)"
if [[ -z "$changed" ]]; then
    echo "No changes detected vs $BASE_REF."
    exit 0
fi

snapshot_changed=0
rfc_changed=0

while IFS= read -r path; do
    [[ -z "$path" ]] && continue
    if [[ "$path" == api/snapshots/* ]]; then
        snapshot_changed=1
    fi
    if [[ "$path" == docs/rfcs/*.md ]]; then
        rfc_changed=1
    fi
done <<< "$changed"

if [[ $snapshot_changed -eq 1 && $rfc_changed -eq 0 ]]; then
    echo "API snapshots changed but no RFC file was added/updated in docs/rfcs/." >&2
    exit 1
fi

echo "RFC gate passed."
