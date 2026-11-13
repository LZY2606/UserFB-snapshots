#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
work=$(mktemp -d "${TMPDIR:-/tmp}/ogg-baseline.XXXXXX")
trap 'rm -rf "$work"' EXIT
cc -std=c11 -O2 -Ichecks/platform -Iinclude checks/baseline.c src/framing.c -o "$work/baseline"
"$work/baseline"
bash checks/legacy.sh "$work"
