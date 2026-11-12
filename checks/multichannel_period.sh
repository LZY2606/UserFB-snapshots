#!/usr/bin/env bash
# Multichannel pitch-period regression driver.
#
# Runs the existing C unit tests, then the C and Java multichannel period
# checks (independent slow AMDF reference, rate/channel grid, full-scale
# arithmetic, history decisions and the public short-stream entry points),
# and finally diffs the cross-language decision vectors.  A zero use count or
# any numeric deviation exits non-zero and prints the first deviation.
set -euo pipefail

root=$(cd "$(dirname "$0")/.." && pwd)
cd "$root"

# mktemp -p keeps the build tree inside TMPDIR, which is required on sandboxed
# systems where the default temporary directory is not writable.
b=$(mktemp -d -p "${TMPDIR:-/tmp}")
trap 'rm -rf "$b"' EXIT

die() {
  echo "MULTICHANNEL_PERIOD_CHECK_FAILED: $*" >&2
  exit 1
}

# 1. Existing C unit tests.
cc -std=c99 -O2 -I. -Itests tests/runtests.c tests/genwave.c sonic.c \
  tests/input_clamping_test.c tests/sonic_api_test.c -lm -o "$b/legacy"
"$b/legacy" | tee "$b/legacy.log"
grep -q "All tests passed." "$b/legacy.log" \
  || die "legacy C tests did not report success"

# 2. C multichannel checks.
cc -std=c99 -O2 -I. checks/multichannel_period.c -lm -o "$b/multichannel_c"
"$b/multichannel_c" 2>&1 | tee "$b/c.log"
read -r cCases cFailures < <(sed -n \
  's/^C_SUMMARY cases=\([0-9][0-9]*\) failures=\([0-9][0-9]*\)$/\1 \2/p' \
  "$b/c.log") || true
[ -n "${cCases:-}" ] || die "missing C summary line"
[ "$cCases" -gt 0 ] || die "C check ran zero cases"
[ "$cFailures" -eq 0 ] || {
  grep -m1 "FIRST_DEVIATION" "$b/c.log" >&2 || true
  die "C check reported $cFailures failures"
}

# 3. Java multichannel checks.
javac -d "$b" Sonic.java checks/MultichannelPeriod.java
java -cp "$b" sonic.MultichannelPeriod 2>&1 | tee "$b/j.log"
read -r jCases jFailures < <(sed -n \
  's/^JAVA_SUMMARY cases=\([0-9][0-9]*\) failures=\([0-9][0-9]*\)$/\1 \2/p' \
  "$b/j.log") || true
[ -n "${jCases:-}" ] || die "missing Java summary line"
[ "$jCases" -gt 0 ] || die "Java check ran zero cases"
[ "$jFailures" -eq 0 ] || {
  grep -m1 "FIRST_DEVIATION" "$b/j.log" >&2 || true
  die "Java check reported $jFailures failures"
}

# 4. Cross-language agreement: identical integer inputs must yield identical
#    candidates and history decisions in both implementations.  Streaming
#    frame counts can differ between the two ports by design, so only the
#    period-decision vectors are compared here; each stream suite already
#    checks its own lengths, sample deltas and steady-state frequency.
grep -E '^XL (GRID|HISTORY|CANDIDATE) ' "$b/c.log" | sed 's/ tag=C$//' \
  | sort > "$b/xl_c.txt"
grep -E '^XL (GRID|HISTORY|CANDIDATE) ' "$b/j.log" | sed 's/ tag=JAVA$//' \
  | sort > "$b/xl_j.txt"
cXl=$(wc -l < "$b/xl_c.txt" | tr -d ' ')
jXl=$(wc -l < "$b/xl_j.txt" | tr -d ' ')
[ "$cXl" -gt 0 ] || die "C check produced no cross-language vectors"
[ "$jXl" -gt 0 ] || die "Java check produced no cross-language vectors"
[ "$cXl" -eq "$jXl" ] \
  || die "cross-language vector count differs: C=$cXl Java=$jXl"
if ! diff -u "$b/xl_c.txt" "$b/xl_j.txt" > "$b/xl.diff"; then
  head -20 "$b/xl.diff" >&2
  die "C and Java period decisions diverge"
fi

# 5. Public streaming cases must actually have executed in both suites.
cStreams=$(grep -c '^XL STREAM impl=C ' "$b/c.log" || true)
jStreams=$(grep -c '^XL STREAM impl=JAVA ' "$b/j.log" || true)
[ "$cStreams" -gt 0 ] || die "C public-stream cases did not run"
[ "$jStreams" -gt 0 ] || die "Java public-stream cases did not run"

echo "MULTICHANNEL_PERIOD_COUNTS legacy=1 c_cases=$cCases java_cases=$jCases cross_vectors=$cXl c_streams=$cStreams java_streams=$jStreams"
echo SONIC_MULTICHANNEL_PERIOD_OK implementations=2
