set -euo pipefail
mkdir -p checks/.tmp
b=$(mktemp -d checks/.tmp/base.XXXXXX)
trap 'rm -rf "$b"' EXIT
for opt in 0 2; do
 cc -std=c11 -O"$opt" -ffp-contract=off -I . checks/baseline.c -pthread -lm -o "$b/check"
 "$b/check" | tee "$b/base$opt.out"
 grep -Fx 'BASELINE_OK' "$b/base$opt.out" >/dev/null
 grep -E '^SERIAL_WITNESS vertices=396 fragments=[1-9][0-9]* foreign_threads=0 pixels=[1-9][0-9]* checksum=[0-9]+$' "$b/base$opt.out" >/dev/null
done
cmp "$b/base0.out" "$b/base2.out"
cc -std=c99 -O2 -I . checks/draw_baseline.c -lm -o "$b/draw"
"$b/draw"
printf '%s\n' BASELINE_COMPLETE
