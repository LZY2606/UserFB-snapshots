#!/bin/bash
set -euo pipefail
build_case() {
 cp libtta.c libtta.h filter.h "$b/"
 # Upstream checked-in config selects x86 SSE2. Use its existing portable branch.
 sed -e '/^#define CPU_X86/d' -e '/^#define ENABLE_SSE2/d' config.h > "$b/config.h"
 case ${TTA_VARIANT:-normal} in
  normal) ;;
  encoder_k) sed '/void tta_encoder_put_value/,/}.*tta_encoder_put_value/s/k = rice->k0;/k = rice->k0 + 1;/' "$b/libtta.c" > "$b/changed.c"; mv "$b/changed.c" "$b/libtta.c" ;;
  decoder_base) sed 's/value += bit_shift\[rice->k0\];/value += bit_shift[rice->k1];/' "$b/libtta.c" > "$b/changed.c"; mv "$b/changed.c" "$b/libtta.c" ;;
  *) return 9 ;;
 esac
 if [[ ${TTA_INTERNAL:-0} = 1 ]]; then
  cc -std=gnu99 -O2 -fwrapv -Wno-address-of-packed-member -I"$b" "$@" -o "$b/case"
 else
  cc -std=gnu99 -O2 -fwrapv -Wno-address-of-packed-member -I"$b" "$b/libtta.c" "$@" -o "$b/case"
 fi
}
