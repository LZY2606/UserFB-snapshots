#!/usr/bin/env bash
# Mutation witnesses for Oniguruma search-budget accounting.
# The script deliberately builds patched copies; it never edits this checkout.
set -u -o pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd)
STAMP=$(date +%Y%m%d-%H%M%S)
OUT=${REDOS_MUTATION_OUTPUT:-"$ROOT/test/redos_mutation_artifacts/$STAMP-$$"}
mkdir -p "$OUT/patches" "$OUT/logs" "$OUT/copies" "$OUT/artifacts"

say() { printf '%s\n' "$*"; }
die() { printf 'MUTATION_TEST_FAIL: %s\n' "$*" >&2; exit 1; }

say "MUTATION_OUTPUT=$OUT"

copy_source() {
  local dst=$1
  mkdir -p "$dst"
  tar -C "$ROOT" \
    --exclude='./.git' \
    --exclude='./build' \
    --exclude='./.budget-logs' \
    --exclude='./.budget-failures' \
    --exclude='./test/redos_mutation_artifacts' \
    -cf - . | tar -C "$dst" -xf -
}

RETRY_PATCH="$OUT/patches/0001-discard-retry-accumulation.patch"
CALLS_PATCH="$OUT/patches/0002-reset-subexp-calls-per-start.patch"

cat >"$RETRY_PATCH" <<'PATCH_EOF'
--- a/src/regexec.c
+++ b/src/regexec.c
@@ -4693,7 +4693,7 @@
               retry_in_match_counter, msa->retry_limit_in_search_counter);
     }
 #endif
-    msa->retry_limit_in_search_counter += retry_in_match_counter;
+    /* MUTATION: discard retry consumption of the completed match. */
   }
 #endif
PATCH_EOF

cat >"$CALLS_PATCH" <<'PATCH_EOF'
--- a/src/regexec.c
+++ b/src/regexec.c
@@ -5719,5 +5719,6 @@

 #define MATCH_AND_RETURN_CHECK(upper_range) \
+  msa.subexp_call_in_search_counter = 0; /* MUTATION: reset per start */ \
   r = match_at(reg, str, end, (upper_range), s, &msa);\
   if (r != ONIG_MISMATCH) {\
     if (r >= 0) {\
PATCH_EOF

NORMAL_DIR="$OUT/copies/normal"
RETRY_DIR="$OUT/copies/retry-discard"
CALLS_DIR="$OUT/copies/call-reset"
copy_source "$NORMAL_DIR"
copy_source "$RETRY_DIR"
copy_source "$CALLS_DIR"

apply_patch() {
  local dir=$1 patch=$2 name=$3 rc
  set +e
  (cd "$dir" && patch -p1 --batch --forward) <"$patch" \
    >"$OUT/logs/$name.patch-apply" 2>&1
  rc=$?
  set -u
  [ "$rc" -eq 0 ] || { cat "$OUT/logs/$name.patch-apply" >&2; die "patch apply failed: $name"; }
}
apply_patch "$RETRY_DIR" "$RETRY_PATCH" retry
apply_patch "$CALLS_DIR" "$CALLS_PATCH" calls

grep -q 'MUTATION: discard retry' "$RETRY_DIR/src/regexec.c" \
  || die 'retry mutation not present in patched source'
grep -q 'MUTATION: reset per start' "$CALLS_DIR/src/regexec.c" \
  || die 'calls mutation not present in patched source'

# run_capture name dir -- command...
run_capture() {
  local name=$1 dir=$2
  shift 2
  local out="$OUT/logs/$name.stdout"
  local err="$OUT/logs/$name.stderr"
  local rc build_log
  set +e
  (
    cd "$dir" || exit 126
    REDOS_ARTIFACT_DIR="$OUT/artifacts/$name" "$@"
  ) >"$out" 2>"$err"
  rc=$?
  set -u
  printf '%s\n' "$rc" >"$OUT/logs/$name.exit"
  build_log=$(sed -n 's/^BUILD_LOG_DIR=//p' "$out" | head -n 1)
  if [ -n "$build_log" ] && [ -d "$build_log" ]; then
    rm -rf "$OUT/logs/$name-build"
    cp -R "$build_log" "$OUT/logs/$name-build"
  fi
  return "$rc"
}

assert_clean_target_compile() {
  local name=$1
  local compile_exit="$OUT/logs/$name-build/target-compile.exit"
  [ -f "$compile_exit" ] || die "$name: missing target-compile.exit"
  [ "$(cat "$compile_exit")" = "0" ] || die "$name: target compilation failed"
  if grep -Eiq '(^|[^[:alnum:]_])skip([:[:space:]]|$)' \
       "$OUT/logs/$name.stdout" "$OUT/logs/$name.stderr"; then
    die "$name: output contained a skip marker"
  fi
  if grep -q 'TARGET_SKIP' "$OUT/logs/$name.stdout"; then
    die "$name: target reported TARGET_SKIP"
  fi
}

assert_nonzero_singleton_failure() {
  local name=$1 group=$2 case=$3
  [ "$(cat "$OUT/logs/$name.exit")" = "1" ] \
    || die "$name: expected exit code 1, got $(cat "$OUT/logs/$name.exit")"
  assert_clean_target_compile "$name"
  grep -q 'ONIG_BUDGET_BASELINE_OK' "$OUT/logs/$name.stdout" \
    || die "$name: mutant no longer completed the baseline success witness"
  grep -q 'ONIG_EXISTING_SYNTAX_OK' "$OUT/logs/$name.stdout" \
    || die "$name: syntax suite did not complete"
  grep -q "^BEGIN $group seed=2677 case=$case\$" "$OUT/logs/$name.stdout" \
    || die "$name: singleton target did not start"
  grep -Eq "^CASE_FAIL group=$group seed=2677 case=$case rc=0 checks=[1-9][0-9]* failures=[1-9]" \
    "$OUT/logs/$name.stdout" \
    || die "$name: missing non-zero-assertion CASE_FAIL record"
}

say '== verifying unmutated copied source passes full target =='
if ! run_capture normal "$NORMAL_DIR" bash checks/run.sh target; then
  cat "$OUT/logs/normal.stdout" "$OUT/logs/normal.stderr" >&2 || true
  die 'normal copied source did not pass full target'
fi
grep -q '^ONIG_BUDGET_TARGET_OK groups=4 checks=[1-9][0-9]*$' \
  "$OUT/logs/normal.stdout" || die 'normal target final marker missing'

say '== retry-accounting mutation must be detected by search group =='
if run_capture retry-target "$RETRY_DIR" \
     bash checks/run.sh target --group search --case 0; then
  die 'retry accumulation mutation was not detected'
fi
assert_nonzero_singleton_failure retry-target search 0
grep 'BUDGET_FIRST_DIFF' "$OUT/logs/retry-target.stdout" \
  >"$OUT/first-diff-retry.txt" || die 'retry: first difference not saved'
grep -q 'retry expected=-18/ONIGERR_RETRY_LIMIT_IN_SEARCH_OVER@1' \
  "$OUT/first-diff-retry.txt" || die 'retry: expected search-limit verdict missing'
grep -q 'actual=-1/ONIG_MISMATCH@-1' "$OUT/first-diff-retry.txt" \
  || die 'retry: mutated mismatch/abort-start difference missing'
grep -q '^REDUCTION_OK ' "$OUT/logs/retry-target.stdout" \
  || die 'retry: structure reduction did not preserve the witness'
[ -s "$OUT/artifacts/retry-target/reduced.spec" ] \
  || die 'retry: reduced.spec missing'
[ -x "$OUT/artifacts/retry-target/replay.sh" ] \
  || die 'retry: executable replay.sh missing'

say '== replaying reduced retry singleton =='
if run_capture retry-replay "$RETRY_DIR" \
     bash "$OUT/artifacts/retry-target/replay.sh"; then
  die 'retry replay unexpectedly passed on mutated source'
fi
[ "$(cat "$OUT/logs/retry-replay.exit")" = "1" ] \
  || die 'retry replay: expected exit 1'
grep -q 'REPLAY_CHECK_FAIL' "$OUT/logs/retry-replay.stdout" \
  || die 'retry replay: no replayed assertion failure'
grep 'REPLAY_FIRST_DIFF' "$OUT/logs/retry-replay.stdout" \
  >"$OUT/replay-first-diff-retry.txt" || die 'retry replay: first diff missing'
grep -q 'retry expected=-18/ONIGERR_RETRY_LIMIT_IN_SEARCH_OVER@1' \
  "$OUT/replay-first-diff-retry.txt" \
  || die 'retry replay: same expected budget category was not reproduced'
grep -q 'actual=-1/ONIG_MISMATCH@-1' "$OUT/replay-first-diff-retry.txt" \
  || die 'retry replay: same mutated difference was not reproduced'

say '== subexp-call reset mutation must be detected by calls group =='
if run_capture calls-target "$CALLS_DIR" \
     bash checks/run.sh target --group calls --case 0; then
  die 'call-count reset mutation was not detected'
fi
assert_nonzero_singleton_failure calls-target calls 0
grep 'BUDGET_FIRST_DIFF' "$OUT/logs/calls-target.stdout" \
  >"$OUT/first-diff-calls.txt" || die 'calls: first difference not saved'
grep -q 'calls expected=-19/ONIGERR_SUBEXP_CALL_LIMIT_IN_SEARCH_OVER@1' \
  "$OUT/first-diff-calls.txt" || die 'calls: expected call-limit verdict missing'
grep -q 'actual=-1/ONIG_MISMATCH@-1' "$OUT/first-diff-calls.txt" \
  || die 'calls: mutated mismatch/abort-start difference missing'
grep -q '^REDUCTION_OK ' "$OUT/logs/calls-target.stdout" \
  || die 'calls: structure reduction did not preserve the witness'
[ -s "$OUT/artifacts/calls-target/reduced.spec" ] \
  || die 'calls: reduced.spec missing'
[ -x "$OUT/artifacts/calls-target/replay.sh" ] \
  || die 'calls: executable replay.sh missing'

say '== replaying reduced calls singleton =='
if run_capture calls-replay "$CALLS_DIR" \
     bash "$OUT/artifacts/calls-target/replay.sh"; then
  die 'calls replay unexpectedly passed on mutated source'
fi
[ "$(cat "$OUT/logs/calls-replay.exit")" = "1" ] \
  || die 'calls replay: expected exit 1'
grep -q 'REPLAY_CHECK_FAIL' "$OUT/logs/calls-replay.stdout" \
  || die 'calls replay: no replayed assertion failure'
grep 'REPLAY_FIRST_DIFF' "$OUT/logs/calls-replay.stdout" \
  >"$OUT/replay-first-diff-calls.txt" || die 'calls replay: first diff missing'
grep -q 'calls expected=-19/ONIGERR_SUBEXP_CALL_LIMIT_IN_SEARCH_OVER@1' \
  "$OUT/replay-first-diff-calls.txt" \
  || die 'calls replay: same expected budget category was not reproduced'
grep -q 'actual=-1/ONIG_MISMATCH@-1' "$OUT/replay-first-diff-calls.txt" \
  || die 'calls replay: same mutated difference was not reproduced'

cat >"$OUT/SUMMARY.txt" <<EOF
normal_exit=0
retry_target_exit=$(cat "$OUT/logs/retry-target.exit")
retry_replay_exit=$(cat "$OUT/logs/retry-replay.exit")
calls_target_exit=$(cat "$OUT/logs/calls-target.exit")
calls_replay_exit=$(cat "$OUT/logs/calls-replay.exit")
retry_first_diff=$(cat "$OUT/first-diff-retry.txt")
calls_first_diff=$(cat "$OUT/first-diff-calls.txt")
patch_retry=$RETRY_PATCH
patch_calls=$CALLS_PATCH
EOF

say "MUTATION_TESTS_OK normal=pass retry=detected+replayed calls=detected+replayed"
say "ARTIFACTS=$OUT"
