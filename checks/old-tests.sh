set -euo pipefail
mkdir -p checks/.tmp
b=$(mktemp -d checks/.tmp/old.XXXXXX)
trap 'rm -rf "$b"' EXIT
for t in test_core_profile_mips; do
 cc -std=c99 -O2 -I . "testing/$t.c" -lm -o "$b/$t"
 "$b/$t" | tee "$b/$t.out"
 grep -Fx "$t: all checks passed" "$b/$t.out" >/dev/null
done
cc -std=c99 -O2 -I . checks/guard_baseline.c -lm -o "$b/guard"
"$b/guard" | tee "$b/guard.out"
grep -Fx 'UPSTREAM_GUARD_OK cases=4 checks=11' "$b/guard.out" >/dev/null
printf '%s\n' OLD_TESTS_OK
