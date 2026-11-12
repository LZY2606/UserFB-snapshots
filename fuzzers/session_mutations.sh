#!/bin/sh
# session_mutations.sh — prove that the fuzzers/session_contract.c regression
# covers the response-chaining gap (HEAD skip-body leaking into connection
# reuse) by running it against a degenerate parser variant.
#
# The variant keeps the parse-stage skip-body behaviour (on_headers_complete
# returning 1 still skips the body) but the EOF-requirement judgement no
# longer takes that skip into account: http_message_needs_eof() drops its
# F_SKIPBODY clause.  On the variant a HEAD response without a length makes
# the connection look dead after message_complete, so the *successor*
# response on the same connection can no longer be parsed.
#
# Detection criteria (all required):
#   - the new regression builds and passes on the original sources;
#   - the variant builds, and the new regression fails on it with a first
#     deviation about HEAD-successor content, completion counts, or the
#     unconsumed tail (a bare keep-alive boolean does not count);
#   - the variant still passes baseline/run.sh (witness: chunked, EOF and
#     fixed-length GET bodies) and the full old strict test.c.
# A build failure, a crash, or a missing file is NOT a detection.
#
# Everything happens in a temporary copy; the checked-out sources are
# never modified.  Only C and POSIX shell are used.

set -u

here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
root=$(CDPATH= cd -- "$here/.." && pwd)

note() { printf '%s\n' "$*"; }
die()  { note "SESSION_MUTATION_ERROR $*" >&2; exit 2; }

for f in http_parser.c http_parser.h test.c \
         baseline/run.sh baseline/witness.c baseline/session_driver.h \
         baseline/target_driver.c fuzzers/session_contract.c; do
  [ -f "$root/$f" ] || die "missing file: $f"
done

work=$(mktemp -d "${TMPDIR:-/tmp}/session-mut.XXXXXX") || die "mktemp failed"
trap 'rm -rf "$work"' EXIT

mkdir -p "$work/orig/baseline" "$work/orig/fuzzers" \
         "$work/mut/baseline"  "$work/mut/fuzzers" || die "mkdir failed"
for f in http_parser.c http_parser.h test.c \
         baseline/run.sh baseline/witness.c baseline/session_driver.h \
         baseline/target_driver.c fuzzers/session_contract.c; do
  cp "$root/$f" "$work/orig/$f" || die "cp $f failed"
  cp "$root/$f" "$work/mut/$f"  || die "cp $f failed"
done

CC=${CC:-cc}
CFLAGS="-std=c99 -O1 -DHTTP_PARSER_STRICT=1"

build_target() ( cd "$1" && $CC $CFLAGS -I. \
  http_parser.c fuzzers/session_contract.c baseline/target_driver.c \
  -o target )

# ---------------------------------------------------------------- original
if build_target "$work/orig" >"$work/orig_build.log" 2>&1; then
  orig_build=0
else
  orig_build=1
fi
orig_test=-1
orig_dev=""
if [ "$orig_build" -eq 0 ]; then
  ( cd "$work/orig" && ./target ) >"$work/orig_test.log" 2>&1
  orig_test=$?
  orig_dev=$(grep -m1 '^SESSION_DEVIATION' "$work/orig_test.log" 2>/dev/null || :)
fi

# ------------------------------------------------------------------ mutant
mut_src="$work/mut/http_parser.c"
anchor=$(grep -n 'F_SKIPBODY) {.*response to a HEAD request' "$mut_src" | cut -d: -f1)
[ -n "$anchor" ] || die "mutation anchor not found in http_message_needs_eof"
before=$(grep -c 'F_SKIPBODY' "$mut_src")
[ "$before" -eq 4 ] || die "unexpected F_SKIPBODY count: $before"
prev=$((anchor - 1))
sed -i.mutbak \
  -e "${anchor}d" \
  -e "${prev}s/||/) {/" \
  "$mut_src" || die "sed mutation failed"
rm -f "$mut_src.mutbak"

# the EOF judgement must no longer mention the skip-body flag ...
awk '/^http_message_needs_eof/,/^}/' "$mut_src" | grep -q 'F_SKIPBODY' &&
  die "mutation did not remove F_SKIPBODY from http_message_needs_eof"
# ... while the parse-stage skip behaviour stays untouched
after=$(grep -c 'F_SKIPBODY' "$mut_src")
[ "$after" -eq 3 ] || die "parse-stage skip-body behaviour not retained"
grep -q 'parser->flags |= F_SKIPBODY' "$mut_src" ||
  die "headers_complete skip-body assignment missing in variant"

if build_target "$work/mut" >"$work/mut_build.log" 2>&1; then
  mut_build=0
else
  mut_build=1
fi
mut_test=-1
mut_dev=""
if [ "$mut_build" -eq 0 ]; then
  ( cd "$work/mut" && ./target ) >"$work/mut_test.log" 2>&1
  mut_test=$?
  mut_dev=$(grep -m1 '^SESSION_DEVIATION' "$work/mut_test.log" 2>/dev/null || :)
fi

# the variant must still pass the pre-existing body framing checks and the
# full old strict test suite (this includes a plain fixed-length GET)
mut_baseline=-1
if [ "$mut_build" -eq 0 ]; then
  ( cd "$work/mut" && bash baseline/run.sh ) >"$work/mut_baseline.log" 2>&1
  mut_baseline=$?
fi

# ------------------------------------------------------------------ report
note "SESSION_MUTATION orig_build=$orig_build orig_test=$orig_test"
note "SESSION_MUTATION orig_first_deviation='${orig_dev:-none}'"
note "SESSION_MUTATION mut_build=$mut_build mut_test=$mut_test mut_baseline=$mut_baseline"
note "SESSION_MUTATION mut_first_deviation='${mut_dev:-none}'"

if [ "$orig_build" -ne 0 ] || [ "$orig_test" -ne 0 ]; then
  note "SESSION_MUTATION_RESULT=NOT_CLEAN original regression does not pass"
  exit 1
fi
if [ "$mut_build" -ne 0 ]; then
  note "SESSION_MUTATION_RESULT=NOT_DETECTED variant build failed (not a detection)"
  exit 1
fi
case "$mut_test" in
  0) note "SESSION_MUTATION_RESULT=NOT_DETECTED variant passes the new regression"
     exit 1 ;;
  126|127|13[0-9]|14[0-9])
     note "SESSION_MUTATION_RESULT=NOT_DETECTED variant crashed (status $mut_test)"
     exit 1 ;;
esac
if [ -z "$mut_dev" ]; then
  note "SESSION_MUTATION_RESULT=NOT_DETECTED variant failed without a session deviation"
  exit 1
fi
kind=$(printf '%s' "$mut_dev" | sed -n 's/.*kind=\([a-z]*\).*/\1/p')
case "$kind" in
  completion|body|tail) ;;
  *) note "SESSION_MUTATION_RESULT=NOT_DETECTED first deviation kind '$kind' is not HEAD-successor evidence"
     exit 1 ;;
esac
if [ "$mut_baseline" -ne 0 ]; then
  note "SESSION_MUTATION_RESULT=NOT_CLEAN variant breaks baseline/run.sh or strict test.c"
  exit 1
fi

note "SESSION_MUTATION_RESULT=DETECTED kind=$kind"
exit 0
