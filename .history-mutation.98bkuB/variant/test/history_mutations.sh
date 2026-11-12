#!/bin/bash
#
# history_mutations.sh - mutation test for capture-start restoration.
#
# It builds Oniguruma twice from two INDEPENDENT temporary source copies:
#
#   normal  - unmodified repository sources
#   variant - the four stack-unwind sites that restore a group's previous
#             end position when a capture start is undone are changed to
#             invalidate that end instead.
#
# Each copy is configured and built in its own fresh build directory, so the
# variant never reuses the normal static library (or vice versa).  The real
# patch, both clean build logs, every test's stdout/stderr, its exit code and
# the first concrete difference are retained under an evidence directory.
#
# Expected outcome:
#   * normal:  witness, the full four-suite contract and test_syntax pass;
#   * variant: witness still passes, and the direct-reference / real-fallback
#     match controls still match the same whole bytes;
#   * variant: the same branch and repeat contract suites reject it on a
#     concrete capture/history CONTENT difference (not on the return code),
#     including at least one case that still matches the identical bytes.
#
# Compile failures, missing files, signals, timeouts or mere non-matches do
# not count as a valid rejection.  The tracked repository sources are never
# modified.
#
# Only the system C compiler, CMake and the in-repository sources are used.

set -u
set -o pipefail

root=$(cd "$(dirname "$0")/.." && pwd)
cd "$root"

work=$(mktemp -d "$root/.history-mutation.XXXXXX")
normal="$work/normal"
variant="$work/variant"
logs="$work/logs"
bin="$work/bin"
mkdir -p "$logs" "$bin"

PASS_COUNT=0
FAIL_COUNT=0

note() { printf '%s\n' "$*"; }
fail() { printf 'MUTATION TEST FATAL: %s\n' "$*" >&2; FAIL_COUNT=$((FAIL_COUNT+1)); }

# run_log NAME EXPECTED-EXIT -- command...
# Captures stdout+stderr, records the real exit code, and only treats an
# unexpected code as a failure (the variant contract runs are expected to
# fail, so set -e must not abort the script on them).
run_log() {
  name=$1; want=$2; shift 2
  [ "$1" = "--" ] && shift
  "$@" >"$logs/$name.out" 2>&1
  code=$?
  printf '%s\n' "$code" >"$logs/$name.exit"
  if [ "$code" -eq "$want" ]; then
    note "  ok   $name (exit=$code)"
    PASS_COUNT=$((PASS_COUNT+1))
  else
    fail "$name expected exit $want but got $code (see $logs/$name.out)"
  fi
  return 0
}

first_difference() {
  # print the first "first difference:" line from a captured log, if any
  sed -n 's/^first difference: //p' "$1" 2>/dev/null | head -1
}

cleanup_on_error_trap() {
  code=$?
  if [ "$code" -ne 0 ]; then
    note "evidence retained at: $work"
  fi
}
trap cleanup_on_error_trap EXIT

# --------------------------------------------------------------------------
# 1. Two independent source copies (fresh trees, no shared build artefacts).
# --------------------------------------------------------------------------
note "== creating independent source copies in $work =="
mkdir -p "$normal" "$variant"
base=$(basename "$work")
tar --exclude="./.git" \
    --exclude="./$base" \
    --exclude="./.history-baseline.*" \
    --exclude="./.history-target.*" \
    --exclude="./.mutexp" \
    --exclude="./.compile-check" \
    --exclude="./.claude" \
    -cf - -C "$root" . | tar -xf - -C "$normal"
tar -C "$normal" -cf - . | tar -xf - -C "$variant"

# --------------------------------------------------------------------------
# 2. The mutation, applied only to the variant copy.
#
#    Every STK_MEM_START unwind block restores both the previous start and
#    the previous end of that group:
#        mem_end_stk[stk->zid] = stk->u.mem.prev_end;
#    The variant instead clears the restored end, modelling a matcher that
#    forgets the group's prior end position when a capture start is undone.
# --------------------------------------------------------------------------
awk '
{
  if ($0 ~ /stk->type == STK_MEM_START/) inms=1
  if (inms && $0 ~ /mem_end_stk\[stk->zid\][ ]*=[ ]*stk->u\.mem\.prev_end;/) {
    sub(/mem_end_stk\[stk->zid\][ ]*=[ ]*stk->u\.mem\.prev_end;/,
        "mem_end_stk[stk->zid].i = INVALID_STACK_INDEX; /*MUT*/")
    mut++
    inms=0
  }
  print
}
END {
  if (mut != 4) { print "MUTATION_SITE_COUNT=" mut > "/dev/stderr"; exit 9 }
  print "MUTATION_SITE_COUNT=" mut > "/dev/stderr"
}
' "$normal/src/regexec.c" > "$variant/src/regexec.c" 2>"$logs/mutation.awk"

site_count=$(sed -n 's/^MUTATION_SITE_COUNT=//p' "$logs/mutation.awk")
if [ "$site_count" != "4" ]; then
  fail "expected exactly 4 mutation sites, got '$site_count'"
  exit 1
fi
diff -u "$normal/src/regexec.c" "$variant/src/regexec.c" \
      > "$logs/mutation.patch" || true
note "  mutation patch saved: $logs/mutation.patch ($site_count sites)"

# the tracked source must be byte-identical to the normal copy
if ! diff -q "$root/src/regexec.c" "$normal/src/regexec.c" >/dev/null; then
  fail "repository source regexec.c was modified"
  exit 1
fi

# --------------------------------------------------------------------------
# 3. Two clean, independent CMake builds (separate build trees and libs).
# --------------------------------------------------------------------------
build_copy() {
  src=$1; tag=$2
  b="$src/build"
  cmake -S "$src" -B "$b" \
        -DBUILD_SHARED_LIBS=OFF -DBUILD_TEST=ON -DCMAKE_BUILD_TYPE=Release \
        >"$logs/$tag.configure.log" 2>&1
  cmake --build "$b" --target onig test_syntax -j 2 \
        >"$logs/$tag.build.log" 2>&1
  [ -f "$b/libonig.a" ]
}
note "== building normal copy =="
if ! build_copy "$normal" normal; then
  fail "normal build failed; see $logs/normal.build.log"
  exit 1
fi
note "== building variant copy =="
if ! build_copy "$variant" variant; then
  fail "variant build failed; see $logs/variant.build.log"
  exit 1
fi

# prove the two static libraries are distinct files, independently produced
if [ "$normal/build/libonig.a" -ef "$variant/build/libonig.a" ]; then
  fail "variant reused the normal static library"
  exit 1
fi
# compile/link logs must be free of compiler diagnostics
for tag in normal variant; do
  if grep -Eq '(^|[[:space:]])(error|warning):' "$logs/$tag.build.log"; then
    fail "$tag build log is not clean: $logs/$tag.build.log"
  fi
done

# --------------------------------------------------------------------------
# 4. Compile the fixed driver, the model/contract objects, witness and a
#    small generated match-control program against each library.
# --------------------------------------------------------------------------
cc -std=c99 -O1 -Isrc -c test/history_model.c    -o "$bin/model.o"
cc -std=c99 -O1 -Isrc -c test/history_contract.c -o "$bin/contract.o"

cat > "$bin/match_control.c" <<'CTRL'
/* Generated by history_mutations.sh: normal-match controls that must keep
 * matching the same whole bytes even after the restoration mutation. */
#include <stdio.h>
#include <string.h>
#include "oniguruma.h"

static int matches(const char *pat, const char *str) {
  OnigSyntaxType syn; OnigErrorInfo ei; OnigRegex re; OnigRegion *r;
  onig_copy_syntax(&syn, ONIG_SYNTAX_DEFAULT);
  onig_set_syntax_op2(&syn,
      onig_get_syntax_op2(&syn) | ONIG_SYN_OP2_ATMARK_CAPTURE_HISTORY);
  if (onig_new(&re, (UChar*)pat, (UChar*)(pat + strlen(pat)), 0,
               ONIG_ENCODING_ASCII, &syn, &ei) != 0) return -1;
  r = onig_region_new();
  int rc = onig_search(re, (UChar*)str, (UChar*)(str + strlen(str)),
                       (UChar*)str, (UChar*)(str + strlen(str)), r, 0);
  onig_region_free(r, 1); onig_free(re);
  return rc >= 0 ? 0 : 1;
}

int main(void) {
  OnigEncoding enc = ONIG_ENCODING_ASCII;
  onig_initialize(&enc, 1);
  static const char *ctrl_backref =
      "\\A(?@<p>a(?:\\g<p>)*b)(?:\\k<p>|(?@<f>ab))\\z";          /* no probe */
  static const char *branch_test =
      "\\A(?@<p>a(?:\\g<p>)*b)(?:\\g<p>X|\\k<p>|(?@<f>ab))\\z";
  static const char *repeat_pat =
      "\\A(?@<p>a(?:\\g<p>)*b)(?:(\\g<p>)+\\k<p>\\z|(?@<f>ab))\\z";
  struct { const char *tag, *pat, *str; } cases[] = {
    {"direct-backref-leaf",    ctrl_backref, "abab"},
    {"direct-backref-nested",  ctrl_backref, "aabbaabb"},
    {"real-fallback-branch",   branch_test,  "aabbab"},
    {"rejected-but-matched",   branch_test,  "abab"},   /* same bytes, bad capture */
    {"real-fallback-repeat",   repeat_pat,   "abab"},
  };
  int bad = 0;
  for (unsigned i = 0; i < sizeof(cases)/sizeof(cases[0]); i++) {
    int rc = matches(cases[i].pat, cases[i].str);
    printf("control %-24s str=%-8s match=%s\n",
           cases[i].tag, cases[i].str, rc == 0 ? "yes" : "NO");
    if (rc != 0) bad = 1;
  }
  onig_end();
  puts(bad ? "MATCH_CONTROL_FAIL" : "MATCH_CONTROL_OK");
  return bad;
}
CTRL

for tag in normal variant; do
  lib="$work/$tag/build/libonig.a"
  cc -std=c99 -O1 -Isrc baseline/target_driver.c \
        "$bin/model.o" "$bin/contract.o" "$lib" -o "$bin/target-$tag"
  cc -std=c99 -O1 -Isrc baseline/witness.c "$lib" -o "$bin/witness-$tag"
  cc -std=c99 -O1 -I"$work/$tag/src" "$bin/match_control.c" "$lib" \
        -o "$bin/match-control-$tag"
done

# --------------------------------------------------------------------------
# 5. Normal build: everything must pass.
# --------------------------------------------------------------------------
note "== normal library expectations =="
run_log normal-witness        0 -- "$bin/witness-normal"
run_log normal-contract-all   0 -- "$bin/target-normal"
run_log normal-match-control  0 -- "$bin/match-control-normal"
run_log normal-test-syntax    0 -- "$work/normal/build/test/test_syntax"

# --------------------------------------------------------------------------
# 6. Variant build: witness and match controls pass; branch/repeat contracts
#    must be rejected on concrete content differences.
# --------------------------------------------------------------------------
note "== variant library expectations =="
run_log variant-witness        0 -- "$bin/witness-variant"
run_log variant-model-suite    0 -- "$bin/target-variant" --suite model
run_log variant-match-control  0 -- "$bin/match-control-variant"

# these two are EXPECTED to be rejected by the new assertions
run_log variant-branch-rejected 1 -- "$bin/target-variant" --suite branch
run_log variant-repeat-rejected 1 -- "$bin/target-variant" --suite repeat

# --------------------------------------------------------------------------
# 7. Validate the quality of the rejections (no weak/accidental failures).
# --------------------------------------------------------------------------
note "== validating rejection evidence =="
for name in variant-branch-rejected variant-repeat-rejected; do
  out="$logs/$name.out"
  diff_line=$(first_difference "$out")
  if [ -z "$diff_line" ]; then
    fail "$name has no concrete 'first difference' line"
    continue
  fi
  case "$diff_line" in
    reg\[*|hist\[*|event\[*]*)
      note "  ok   $name content difference: $diff_line" ;;
    *)
      fail "$name difference is not a capture/history content diff: $diff_line"
      continue ;;
  esac
  # a mere non-match would say "expected match but search failed"
  if grep -q 'expected match but search failed' "$out"; then
    fail "$name only proves non-matching, which is not acceptable"
  fi
  # at least one rejected plan expected a match and differs only in content
  if ! grep -q 'expect_match=1' "$out"; then
    fail "$name contains no rejected case that matched the whole input"
  fi
done

# the rejected-but-matched control proves identical bytes still match
if ! grep -q 'rejected-but-matched .* match=yes' \
        "$logs/variant-match-control.out"; then
  fail "variant fails to match the same whole bytes on a rejected case"
fi

# --------------------------------------------------------------------------
# 8. Summary and retained evidence.
# --------------------------------------------------------------------------
{
  echo "mutation evidence: $work"
  echo "patch:             $logs/mutation.patch"
  for f in normal variant; do
    echo "build logs:        $logs/$f.configure.log $logs/$f.build.log"
  done
  for f in normal-witness normal-contract-all normal-match-control \
           normal-test-syntax variant-witness variant-model-suite \
           variant-match-control variant-branch-rejected \
           variant-repeat-rejected; do
    printf 'exit %-26s %s  first-diff: %s\n' \
      "$f" "$(cat "$logs/$f.exit")" "$(first_difference "$logs/$f.out")"
  done
} | tee "$logs/SUMMARY.txt"

note "logs, patch, stdout/stderr and exit codes retained in: $logs"
if [ "$FAIL_COUNT" -ne 0 ]; then
  note "MUTATION TEST FAILED ($FAIL_COUNT problem(s))"
  exit 1
fi
note "MUTATION TEST OK: normal passes; variant kept matching but was rejected on history content"
exit 0
