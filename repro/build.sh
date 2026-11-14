#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mkdir -p .task-build/obj
flags=(-std=c99 -O2 -UNDEBUG -I. -Iinclude -Ilib -Ithird_party/ogg/include)
sources=(apiwrapper bitpack decapiwrapper decinfo decode dequant fragment huffdec idct info internal quant state analyze fdct encfrag encapiwrapper encinfo encode enquant huffenc mathops mcenc rate tokenize)
for name in "${sources[@]}"; do
  src="lib/$name.c"; obj=".task-build/obj/$name.o"
  cc "${flags[@]}" -c "$src" -o "$obj"
done
for name in bitwise framing; do
  src="third_party/ogg/src/$name.c"; obj=".task-build/obj/ogg_$name.o"
  cc "${flags[@]}" -c "$src" -o "$obj"
done
ar rcs .task-build/libcodec.a .task-build/obj/*.o
