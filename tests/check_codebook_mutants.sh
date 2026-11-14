#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
test -s tests/codebook_contract.c
sandbox=$(mktemp -d)
trap 'rm -rf "$sandbox"' EXIT
for variant in merged forward; do
  dest="$sandbox/$variant"
  mkdir -p "$dest"
  tar --exclude='./.git' --exclude='./.task-build' -cf - . | tar -xf - -C "$dest"
  (cd "$dest"; patch -s -p1 <"tests/mutants/$variant.patch")
  set +e
  (cd "$dest"; bash task_support/run.sh target) >"$sandbox/$variant.log" 2>&1
  code=$?
  set -e
  if [[ $code != 4 ]] || ! grep -q '^ILBC_TARGET_LINK_OK$' "$sandbox/$variant.log" || ! grep -q 'CONTENT_MISMATCH' "$sandbox/$variant.log"; then
    cat "$sandbox/$variant.log"
    printf 'MUTANT_REJECT_FAILED variant=%s exit=%s\n' "$variant" "$code" >&2
    exit 1
  fi
  grep 'CONTENT_MISMATCH' "$sandbox/$variant.log" | head -1
  printf 'MUTANT_CONTENT_REJECTED variant=%s exit=4\n' "$variant"
done
printf 'ILBC_MUTANTS_OK variants=2\n'
