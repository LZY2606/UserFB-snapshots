#!/usr/bin/env bash
set -euo pipefail
# Usage: build.sh SOURCE_ROOT TEST_C OUTPUT
src=$1
probe=$2
out=$3
b=$(mktemp -d)
mkdir -p "$b/speex"
cat > "$b/speex/speex_config_types.h" <<'TYPES'
#include <stdint.h>
typedef int16_t spx_int16_t;
typedef uint16_t spx_uint16_t;
typedef int32_t spx_int32_t;
typedef uint32_t spx_uint32_t;
TYPES
files=(cb_search exc_10_32_table exc_8_128_table filters gain_table hexc_table high_lsp_tables lsp ltp speex stereo vbr vq bits exc_10_16_table exc_20_32_table exc_5_256_table exc_5_64_table gain_table_lbr hexc_10_32_table lpc lsp_tables_nb modes modes_wb nb_celp quant_lsp sb_celp speex_callbacks speex_header window smallft)
args=()
for f in "${files[@]}";do args+=("$src/libspeex/$f.c");done
cc -std=c99 -O2 -DFLOATING_POINT -DVAR_ARRAYS -DUSE_SMALLFT -DEXPORT= -I"$b" -I"$b/speex" -I"$src/include" "${args[@]}" "$probe" -lm -o "$out"
