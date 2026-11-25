#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mkdir -p .task-build/obj
flags=(-std=c99 -O2 -UNDEBUG -I. -Iinclude -Ilib -Ithird_party/ogg/include)
#The independent residual regression test asks for pre-transform coefficients
#and pre-filter pixels through observation hooks in lib/decode.c.
#They are compiled in only when TASK_OBSERVE=1; a plain build never defines
#OC_TASK_OBSERVE and exports no debug interface.
if [[ ${TASK_OBSERVE:-0} == 1 ]]; then
  flags+=(-DOC_TASK_OBSERVE)
fi
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
