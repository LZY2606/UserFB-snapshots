#!/usr/bin/env bash
#
# Layout contract driver.
#
#   bash tests/run-layout-contract.sh             # normal run only
#   bash tests/run-layout-contract.sh --mutations # normal run, then three
#                                                 # independent degenerate
#                                                 # library builds, each of
#                                                 # which must be caught by a
#                                                 # concrete test assertion.
#
# The test binary itself is mode-agnostic: assertions never change between the
# normal and mutation phases. This script only swaps the library source under
# test, rebuilds, and records what the unchanged test binary catches. No shader
# compiler, GPU, network or test framework is required.

set -euo pipefail

# Always operate from the repository root regardless of the caller's cwd.
cd "$(dirname "$0")/.."

# Evidence is an output of this driver; never let a stale copy influence a run.
rm -f tests/layout-evidence.json

mode="normal"
if [[ "${1:-}" == "--mutations" ]]; then
  mode="mutations"
fi

# Explicit template under $TMPDIR: a bare mktemp may target a system temp path
# that is unavailable in restricted sandboxes.
work=$(mktemp -d "${TMPDIR:-/tmp}/layout-contract.XXXXXX")
trap 'rm -rf "$work"' EXIT

# Build spirv_reflect.c + the unchanged contract test against a given tree
# root, then run it. Sets the global variables below.
run_variant() {
  local root=$1 label=$2
  local bdir="$work/build-$label"
  mkdir -p "$bdir"
  RUN_COMPILED=0
  RUN_EXECUTED=0
  RUN_EXIT=0
  RUN_REASON=""
  if cc -std=c99 -I"$root" -c "$root/spirv_reflect.c" -o "$bdir/reflect.o" \
      && c++ -std=c++14 -I"$root" tests/layout_contract.cpp "$bdir/reflect.o" -o "$bdir/lc"; then
    RUN_COMPILED=1
  else
    return 0
  fi
  set +e
  local out
  # Run with the variant tree as cwd so the test's relative material paths and
  # its evidence file resolve inside that independent copy.
  out=$(cd "$root" && "$bdir/lc" "$label" 2>&1)
  RUN_EXIT=$?
  set -e
  # A real assertion failure prints the LAYOUT_CONTRACT_FAIL line and writes
  # evidence; a crash or an unexecuted test does not.
  if [[ -s "$root/tests/layout-evidence.json" ]]; then
    RUN_EXECUTED=1
  fi
  RUN_REASON=${out#LAYOUT_CONTRACT_FAIL }
}

# ---------------------------------------------------------------------------
# Phase 0: the pristine library must pass in full.
# ---------------------------------------------------------------------------
normal_dir="$work/normal"
mkdir -p "$normal_dir"
cp -R spirv_reflect.c spirv_reflect.h include tests checks "$normal_dir/"
rm -f "$normal_dir/tests/layout-evidence.json"
run_variant "$normal_dir" normal
if [[ "$RUN_COMPILED" -ne 1 ]]; then
  echo "FATAL normal build failed" >&2
  exit 1
fi
# A pristine passing run exits zero and writes no failure evidence.
if [[ "$RUN_EXIT" -ne 0 || -s "$normal_dir/tests/layout-evidence.json" ]]; then
  echo "FATAL pristine library must pass before mutations are introduced" >&2
  exit 1
fi
# The contract binary's own summary line (cases/assertions are its real counts).
"$work/build-normal/lc" normal | grep -E '^LAYOUT_NORMAL_OK cases=[1-9][0-9]* assertions=[1-9][0-9]*$'

if [[ "$mode" != "mutations" ]]; then
  exit 0
fi

# ---------------------------------------------------------------------------
# Mutation phase: three fully independent temporary source trees.
#
# Every mutation compiles; each one changes behavior in exactly one way and
# must be stopped by a numeric or byte assertion inside the unchanged test.
# Compile failures, crashes, missing evidence or a zero exit are never counted
# as a catch and fail this driver instead.
# ---------------------------------------------------------------------------

make_tree() {
  local dst=$1
  mkdir -p "$dst"
  cp -R spirv_reflect.c spirv_reflect.h include tests checks "$dst/"
  rm -f "$dst/tests/layout-evidence.json"
}

json_field() {
  # Extract a top-level string field from the tiny evidence document.
  local file=$1 field=$2
  sed -n "s/.*\"$field\": \"\\(.*\\)\",*/\\1/p" "$file" | head -n 1
}
json_number() {
  local file=$1 field=$2
  sed -n "s/.*\"$field\": \\([0-9][0-9]*\\).*/\\1/p" "$file" | head -n 1
}

records=()

run_mutation() {
  local id=$1
  local dst="$work/mut-$id"
  make_tree "$dst"

  case "$id" in
    matrix-major)
      # Confuse matrix rows and columns when computing a matrix's byte size.
      # A placeholder keeps the two substitutions from cancelling each other.
      sed -e 's/numeric\.matrix\.column_count \* p_member_var->numeric\.matrix\.stride/__LAYOUT_SWAP_TOKEN__/' \
          -e 's/numeric\.matrix\.row_count \* p_member_var->numeric\.matrix\.stride/numeric.matrix.column_count * p_member_var->numeric.matrix.stride/' \
          -e 's/__LAYOUT_SWAP_TOKEN__/numeric.matrix.row_count * p_member_var->numeric.matrix.stride/' \
          "$dst/spirv_reflect.c" > "$dst/spirv_reflect.c.new"
      mv "$dst/spirv_reflect.c.new" "$dst/spirv_reflect.c"
      ;;
    nested-offset)
      # Non-array nested members lose their parent's absolute offset.
      sed 's/p_member_var->offset + p_var->absolute_offset/p_member_var->offset/' \
          "$dst/spirv_reflect.c" > "$dst/spirv_reflect.c.new"
      mv "$dst/spirv_reflect.c.new" "$dst/spirv_reflect.c"
      ;;
    bytecode-remap)
      # Keep the in-memory reflection fields but never touch the bytecode.
      sed -e 's/\*p_code = new_binding_number;/*p_code = *p_code;/' \
          -e 's/\*p_code = new_set_binding;/*p_code = *p_code;/' \
          -e 's/\*p_code = new_set_number;/*p_code = *p_code;/' \
          "$dst/spirv_reflect.c" > "$dst/spirv_reflect.c.new"
      mv "$dst/spirv_reflect.c.new" "$dst/spirv_reflect.c"
      ;;
    *)
      echo "FATAL unknown mutation $id" >&2
      exit 1
      ;;
  esac

  # Guard: each mutation must actually have edited the library source.
  if cmp -s spirv_reflect.c "$dst/spirv_reflect.c"; then
    echo "FATAL mutation $id did not modify spirv_reflect.c" >&2
    exit 1
  fi

  run_variant "$dst" "$id"

  if [[ "$RUN_COMPILED" -ne 1 ]]; then
    echo "FATAL mutation $id failed to compile; not counted as caught" >&2
    exit 1
  fi
  if [[ "$RUN_EXIT" -eq 0 || "$RUN_EXIT" -gt 127 ]]; then
    echo "FATAL mutation $id was not caught (exit=$RUN_EXIT)" >&2
    exit 1
  fi
  if [[ "$RUN_EXECUTED" -ne 1 || -z "$RUN_REASON" ]]; then
    echo "FATAL mutation $id produced no executed assertion evidence" >&2
    exit 1
  fi

  local ev="$dst/tests/layout-evidence.json"
  local fixture location assertion cases assertions
  fixture=$(json_field "$ev" fixture)
  location=$(json_field "$ev" location)
  assertion=$(json_field "$ev" assertion)
  cases=$(json_number "$ev" cases_executed)
  assertions=$(json_number "$ev" assertions_executed)
  if [[ -z "$fixture" || -z "$assertion" || "${assertions:-0}" -eq 0 ]]; then
    echo "FATAL mutation $id evidence is incomplete" >&2
    exit 1
  fi

  printf 'MUTATION_CAUGHT id=%s compiled=1 executed=1 assertion=%s\n' "$id" "$RUN_REASON"

  records+=("$(cat <<EOF
  {
    "id": "$id",
    "exit_code": $RUN_EXIT,
    "fixture": "$fixture",
    "first_difference_location": "$location",
    "assertion": "$assertion",
    "cases_executed": $cases,
    "assertions_executed": $assertions
  }
EOF
)")
}

run_mutation matrix-major
run_mutation nested-offset
run_mutation bytecode-remap

# Aggregate the first-difference evidence (fixture, member path / word position
# inside the assertion text, and exit codes) into the repository's evidence file.
normal_summary=$("$work/build-normal/lc" normal)
normal_cases=${normal_summary#LAYOUT_NORMAL_OK cases=}
normal_cases=${normal_cases%% assertions=*}
normal_assertions=${normal_summary##* assertions=}

{
  echo "{"
  echo "  \"normal\": {"
  echo "    \"result\": \"PASS\","
  echo "    \"cases\": $normal_cases,"
  echo "    \"assertions\": $normal_assertions"
  echo "  },"
  echo "  \"mutations\": ["
  for i in 0 1 2; do
    printf '%s' "${records[$i]}"
    if [[ $i -lt 2 ]]; then printf ','; fi
    echo
  done
  echo "  ]"
  echo "}"
} > tests/layout-evidence.json
