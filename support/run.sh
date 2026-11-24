#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
src=$PWD
mode=${1:-baseline}
[[ $mode == baseline || $mode == target ]] || exit 2
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
cd "$work"
if ! "$src/configure" --disable-shared --enable-static >configure.log 2>&1; then cat configure.log; exit 2; fi
if ! make -j2 >build.log 2>&1; then cat build.log; exit 2; fi
cc -std=c11 -O1 -g -I. -I"$src" -I"$src/support" "$src/support/baseline.c" "$src/support/sample_io.c" .libs/libbacktrace.a -o baseline
for sample in A B; do
 ./baseline "$src/support/samples/capture-$sample.elf" "$src/support/samples/capture-$sample.pcs"
done
./baseline "$src/support/samples/optimized-C.elf" "$src/support/samples/optimized-C.pcs"
[[ $mode == target ]] || exit 0
for name in sample_report.c evidence_test.c analysis.md; do
 if [[ ! -s $src/$name ]]; then echo "missing required file: $name" >&2; exit 6; fi
done
cc -std=c11 -O1 -g -I. -I"$src" -I"$src/support" "$src/sample_report.c" "$src/support/report_main.c" "$src/support/sample_io.c" .libs/libbacktrace.a -o sample_report
cc -std=c11 -O1 -g -I. -I"$src" -I"$src/support" "$src/evidence_test.c" "$src/support/test_main.c" "$src/support/sample_io.c" .libs/libbacktrace.a -o evidence_test
out="$src/sample-output"
mkdir -p "$out"
cd "$src"
"$work/evidence_test" | tee "$work/tests.log"
grep -Eq '^EVIDENCE_TESTS_RETURNED checks=[1-9][0-9]*$' "$work/tests.log"
"$work/sample_report" support/samples/capture-A.elf support/samples/capture-A.pcs support/samples/capture-B.elf support/samples/capture-B.pcs "$out/A.tsv" "$out/B.tsv" "$out/A-replay.tsv" | tee "$work/reports.log"
grep -qx 'SAMPLE_REPORTS_RETURNED objects=2 replay=1' "$work/reports.log"
"$work/sample_report" support/samples/optimized-C.elf support/samples/optimized-C.pcs support/samples/capture-B.elf support/samples/capture-B.pcs "$out/C.tsv" "$out/B-again.tsv" "$out/C-replay.tsv" | tee "$work/compiler-report.log"
grep -qx 'SAMPLE_REPORTS_RETURNED objects=2 replay=1' "$work/compiler-report.log"
echo 'SOURCE_EVIDENCE_OK'
