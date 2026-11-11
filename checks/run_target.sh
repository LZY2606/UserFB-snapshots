#!/usr/bin/env bash
set -euo pipefail
for f in checks/derivatives.cpp checks/check_derivatives.sh; do
  [[ -s "$f" ]] || { echo "missing or empty target file: $f" >&2; exit 2; }
done
b=$(mktemp -d "${TMPDIR:-.}/opensubdiv-target.XXXXXX")
trap 'rm -rf "$b"' EXIT
bash checks/check_derivatives.sh > "$b/result.log" 2>&1 || {
  cat "$b/result.log"; exit 1;
}
cat "$b/result.log"
# A skipped/empty scenario must not be accepted by a summary-only run.
if grep -Eiq '(^|[[:space:]])(SKIP|SKIPPED)([[:space:]]|$)' "$b/result.log"; then
  echo 'skipped target cases are not accepted' >&2; exit 3
fi
for name in derivative-reference stencil-identity fvar-sides; do
  [[ $(grep -Ec "^PASS ${name} cases=[1-9][0-9]*$" "$b/result.log") == 1 ]] || {
    echo "missing positive executed case count: $name" >&2; exit 3;
  }
done
for name in derivative-scale mixed-sign fvar-index; do
  [[ $(grep -Ec "^DETECTED ${name} cases=[1-9][0-9]*$" "$b/result.log") == 1 ]] || {
    echo "missing semantic rejection count: $name" >&2; exit 3;
  }
done
[[ $(grep -Ec '^OPENSUBDIV_DERIVATIVES_OK$' "$b/result.log") == 1 ]] || exit 3
