#!/usr/bin/env bash
#
# Build and run the GC state-sequence regression in a temporary directory:
#   - single-file mjs.c amalgamation
#   - split library listed by src/mjs_sources.mk
#   - four temporary mutants (external-root marking and property-value marking,
#     each for both build shapes)
#
# The production sources are never modified.  A mutant only counts as detected
# when its build succeeds and the result binary then fails with a
# content/identity assertion or a sanitizer diagnostic.

set -euo pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd)
WORK=$(mktemp -d "${TMPDIR:-/tmp}/mjs-gcseq.XXXXXX")
trap 'rm -rf "$WORK"' EXIT

CFLAGS=(-std=c99 -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all)
INCS=(-I"$ROOT" -I"$ROOT/src" -I"$ROOT/src/frozen")
DEFS=()
case "$(uname -s)" in
  Darwin) DEFS+=(-D_DARWIN_C_SOURCE) ;;
esac

export ASAN_OPTIONS="detect_leaks=0:abort_on_error=1"
export UBSAN_OPTIONS="print_stacktrace=1:halt_on_error=1"

TEST_SRC=("$ROOT/regress/gc_sequence.c" \
          "$ROOT/regress/gc_sequence_test.c" \
          "$ROOT/regress/gc_model.c")

# Ask the canonical source manifest for the split-library file list.
cat >"$WORK/sources.mk" <<'EOF'
include $(ROOT)/src/mjs_sources.mk
print-split-sources:
	@printf '%s\n' $(MJS_SOURCES) $(COMMON_SOURCES)
EOF
SPLIT_TEXT=$(make -s -f "$WORK/sources.mk" print-split-sources ROOT="$ROOT")
SPLIT_NAMES=()
while IFS= read -r name; do
  [ -n "$name" ] && SPLIT_NAMES+=("$name")
done <<< "$SPLIT_TEXT"
if [ "${#SPLIT_NAMES[@]}" -eq 0 ]; then
  echo "FAIL: src/mjs_sources.mk produced no sources" >&2
  exit 1
fi

build_single() {
  local out=$1
  shift
  clang "${CFLAGS[@]}" "${DEFS[@]}" "${INCS[@]}" "$@" -lm -o "$out"
}

build_split() {
  local out=$1
  local srcroot=$2
  shift 2
  local srcs=()
  local name
  for name in "${SPLIT_NAMES[@]}"; do
    srcs+=("$srcroot/$name")
  done
  clang "${CFLAGS[@]}" "${DEFS[@]}" -DMJS_EXPOSE_PRIVATE "${INCS[@]}" \
        "$@" "${srcs[@]}" -lm -o "$out"
}

run_normal() {
  local label=$1
  local bin=$2
  local log="$WORK/normal-$label.log"
  if ! "$bin" >"$log" 2>&1; then
    echo "FAIL: normal $label execution failed" >&2
    cat "$log" >&2
    exit 1
  fi
  grep -q '^CASE ' "$log"
  grep -q '^INVALID-INPUT ' "$log"
  grep -q 'MJS_GC_SEQUENCE_TEST_OK cases=8 generated=16' "$log"
  echo "NORMAL $label OK"
}

mutate_file() {
  local target=$1
  local kind=$2
  python3 - "$target" "$kind" <<'PY'
import sys
path, kind = sys.argv[1], sys.argv[2]
if kind == "external-roots":
    needle = "gc_mark_mbuf_pt(mjs, &mjs->owned_values);"
    repl = "/* mutant: external roots intentionally unmarked */"
elif kind == "property-values":
    needle = "gc_mark(mjs, &prop->value);"
    repl = "/* mutant: property values intentionally unmarked */"
else:
    raise SystemExit("unknown mutant kind")
text = open(path, "r", encoding="utf-8").read()
count = text.count(needle)
if count != 1:
    raise SystemExit("%s: expected exactly one mutation site, found %d" %
                     (path, count))
open(path, "w", encoding="utf-8").write(text.replace(needle, repl, 1))
PY
}

KINDS=(external-roots property-values)
MODES=(single split)
declare -a KIND_SEEN
KIND_SEEN[0]=0
KIND_SEEN[1]=0

for kind in "${KINDS[@]}"; do
  for mode in "${MODES[@]}"; do
    dir="$WORK/mut-$kind-$mode"
    mkdir -p "$dir"
    cp "$ROOT/mjs.c" "$ROOT/mjs.h" "$dir/"
    cp -R "$ROOT/src" "$dir/src"
    cp -R "$ROOT/regress" "$dir/regress"

    if [ "$mode" = single ]; then
      target="$dir/mjs.c"
    else
      target="$dir/src/mjs_gc.c"
    fi
    if ! mutate_file "$target" "$kind" >"$WORK/mutate.log" 2>&1; then
      echo "FAIL: could not create mutant $kind/$mode" >&2
      cat "$WORK/mutate.log" >&2
      exit 1
    fi

    mtest=("$dir/regress/gc_sequence.c" \
           "$dir/regress/gc_sequence_test.c" \
           "$dir/regress/gc_model.c")
    bin="$WORK/mutant-$kind-$mode"
    if [ "$mode" = single ]; then
      if ! build_single "$bin" "$dir/mjs.c" "${mtest[@]}" \
           >"$WORK/build-$kind-$mode.log" 2>&1; then
        echo "FAIL: mutant $kind/$mode build failed (not a detection)" >&2
        cat "$WORK/build-$kind-$mode.log" >&2
        exit 1
      fi
    else
      if ! build_split "$bin" "$dir/src" "${mtest[@]}" \
           >"$WORK/build-$kind-$mode.log" 2>&1; then
        echo "FAIL: mutant $kind/$mode build failed (not a detection)" >&2
        cat "$WORK/build-$kind-$mode.log" >&2
        exit 1
      fi
    fi

    runlog="$WORK/run-$kind-$mode.log"
    if "$bin" >"$runlog" 2>&1; then
      echo "FAIL: mutant $kind/$mode was not detected" >&2
      exit 1
    fi
    if ! grep -Eq 'ERROR: AddressSanitizer|UndefinedBehaviorSanitizer|runtime error:|MJS_GC_SEQUENCE_MISMATCH|MJS_GC_SEQUENCE_DETERMINISM_FAIL|Assertion failed|Aborted' "$runlog"; then
      echo "FAIL: mutant $kind/$mode failed without a recognizable GC assertion/sanitizer" >&2
      cat "$runlog" >&2
      exit 1
    fi
    echo "MUTANT $kind $mode DETECTED"
    if [ "$kind" = external-roots ]; then
      KIND_SEEN[0]=1
    else
      KIND_SEEN[1]=1
    fi
  done
done

# ---------------------------------------------------------------------------
# Normal builds: same corpus through both library shapes.
# ---------------------------------------------------------------------------
single_bin="$WORK/single"
split_bin="$WORK/split"
build_single "$single_bin" "$ROOT/mjs.c" "${TEST_SRC[@]}"
build_split "$split_bin" "$ROOT/src" "${TEST_SRC[@]}"
run_normal single "$single_bin"
run_normal split "$split_bin"

if [ "${KIND_SEEN[0]}" -ne 1 ] || [ "${KIND_SEEN[1]}" -ne 1 ]; then
  echo "FAIL: both mutant kinds must be detected" >&2
  exit 1
fi

printf 'MJS_GC_SEQUENCE_OK cases=8 mutants=2\n'
