#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
for file in include/ogg/opus_repage.h src/opus_repage.c checks/opus_repage_contract.c; do
  if [[ ! -s "$file" ]]; then echo "MISSING_TARGET $file" >&2; exit 2; fi
done
work=$(mktemp -d "${TMPDIR:-/tmp}/ogg-target.XXXXXX")
trap 'rm -rf "$work"' EXIT
cc -std=c11 -O2 -Wall -Wextra -Ichecks/platform -Iinclude -Ichecks \
  src/framing.c src/opus_repage.c checks/contract_main.c checks/opus_repage_contract.c -o "$work/contract"
set +e
"$work/contract" >"$work/target.stdout" 2>"$work/target.stderr"
result=$?
set -e
cat "$work/target.stdout"
cat "$work/target.stderr" >&2
mkdir -p .check-output
cp "$work/target.stdout" .check-output/target.stdout
cp "$work/target.stderr" .check-output/target.stderr
printf '%s\n' "$result" >.check-output/target.exit
echo "TARGET_PROCESS_EXIT=$result"
if [[ $result != 0 ]]; then exit "$result"; fi
awk -f checks/verify.awk "$work/target.stdout"
bash checks/legacy.sh "$work"
echo 'OPUS_REPAGE_TARGET_COMPLETE'
