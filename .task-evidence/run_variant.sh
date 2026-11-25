#!/bin/bash
# Build a mutated libtheora (observation hooks enabled) and run the residual
# regression suite. Usage: run_variant.sh <variant-dir> <log-path>
set -u
VARIANT_DIR="$1"
LOG="$2"
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"
NAME="$(basename "$VARIANT_DIR")"
OBJ=".task-build/variant-$NAME"
rm -rf "$OBJ"; mkdir -p "$OBJ/obj"
COMMON=(-std=c99 -O2 -UNDEBUG -DOC_TASK_OBSERVE -I. -Iinclude -Ilib -Ithird_party/ogg/include)
OBJS=()
for src in lib/*.c; do
  base="$(basename "$src" .c)"
  # decode.c comes from the variant; encoder_disabled.c conflicts with the
  # real encoder and is excluded (mirroring repro/build.sh).
  if [ "$base" = "decode" ] || [ "$base" = "encoder_disabled" ]; then continue; fi
  cc "${COMMON[@]}" -c "$src" -o "$OBJ/obj/$base.o"
  OBJS+=("$OBJ/obj/$base.o")
done
for src in third_party/ogg/src/framing.c third_party/ogg/src/bitwise.c; do
  base="$(basename "$src" .c)"
  cc "${COMMON[@]}" -c "$src" -o "$OBJ/obj/$base.o"
  OBJS+=("$OBJ/obj/$base.o")
done
cc "${COMMON[@]}" -c "$VARIANT_DIR/decode.c" -o "$OBJ/obj/decode.o"
OBJS+=("$OBJ/obj/decode.o")
cc "${COMMON[@]}" task_support/driver.c tests/residual_reference.c \
  "${OBJS[@]}" -lm -o "$OBJ/target"
set +e
"$OBJ/target" > "$LOG" 2>&1
status=$?
set -e
echo "variant=$NAME exit=$status"
exit $status
