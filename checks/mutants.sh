#!/bin/bash
# Mutation checks for tests/target_regression.c.
#
# Builds three deliberately broken but still compilable copies of emu2413.c
# inside a temporary workspace and verifies that the regression groups reject
# each of them with a numeric assertion failure (a build failure would not
# count). The unmodified sources are run through the same assertions as a
# control. For every mutant class the first differing sample reported by the
# test binary is saved in the workspace.
set -euo pipefail

root=$(cd "$(dirname "$0")/.." && pwd)
work=$(mktemp -d)
echo "MUTANT_WORKSPACE $work"

prepare() { # $1 = destination directory
  mkdir -p "$1/tests" "$1/checks"
  cp "$root/emu2413.c" "$root/emu2413.h" "$1/"
  cp "$root/tests/target_regression.c" "$1/tests/"
  cp "$root/checks/harness.h" "$root/checks/target_main.c" "$1/checks/"
}

build() { # $1 = directory; build failures are script bugs, not mutant kills
  (cd "$1" && cc -std=c17 -O2 -I. -Ichecks tests/target_regression.c checks/target_main.c -lm -o target)
}

# control: the unmodified sources must pass the same assertions
prepare "$work/control"
build "$work/control"
(cd "$work/control" && ./target) >"$work/control.log" 2>&1 || {
  cat "$work/control.log"
  echo "CONTROL_FAILED: unmodified sources did not pass" >&2
  exit 1
}
grep -q TARGET_GROUPS_OK "$work/control.log"
echo "CONTROL_PASS unmodified sources pass the same assertions"

mutant() { # $1 = name, $2 = sed expression applied to emu2413.c
  local dir="$work/$1"
  prepare "$dir"
  sed "$2" "$root/emu2413.c" >"$dir/emu2413.c"
  if cmp -s "$root/emu2413.c" "$dir/emu2413.c"; then
    echo "MUTANT_ERROR $1: sed expression did not change emu2413.c" >&2
    exit 1
  fi
  build "$dir" # must still compile; a build failure would not count
  if (cd "$dir" && ./target) >"$dir.log" 2>&1; then
    echo "MUTANT_SURVIVED $1: regression groups did not reject it" >&2
    exit 1
  fi
  if ! grep -m1 'FIRST-DIFF' "$dir.log" >"$work/$1.first_diff"; then
    cat "$dir.log" >&2
    echo "MUTANT_ERROR $1: failed without a numeric FIRST-DIFF assertion" >&2
    exit 1
  fi
  echo "MUTANT_KILLED $1: $(cat "$work/$1.first_diff")"
}

# 1. changed long-noise feedback term
mutant noise-feedback 's/0x800200/0x800208/'

# 2. one XOR relation of the short noise mistaken for an OR
mutant short-noise-or 's/h_bit2 \^ h_bit7/h_bit2 | h_bit7/'

# 3. snare phase bit shifted by one
mutant snare-phase-bit 's/BIT(slot->pg_out, PG_BITS - 2)/BIT(slot->pg_out, PG_BITS - 3)/'

echo "MUTANTS_OK first-difference records kept in $work"
