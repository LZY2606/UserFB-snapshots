#!/bin/bash
set -euo pipefail
if [[ $# != 2 ]]; then exit 2; fi
binary=$1
receipt=$2
rm -f "$receipt" "$receipt.stdout"
"$binary" "$receipt" > "$receipt.stdout"
cat "$receipt.stdout"
if [[ ! -s $receipt ]] || ! awk 'END {exit !(NR==1 && $0=="LOCATION_GROUPS_COMPLETED=5")}' "$receipt"; then
 echo 'contract did not return through all groups' >&2; exit 2
fi
if awk 'toupper($0) ~ /(^|[^A-Z])(SKIP|SKIPPED)([^A-Z]|$)/ {found=1} END {exit !found}' "$receipt.stdout"; then
 echo 'SKIP is not a completed group' >&2; exit 2
fi
