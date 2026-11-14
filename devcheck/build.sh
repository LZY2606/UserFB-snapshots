#!/bin/bash
set -euo pipefail
lib_sources=(src/add.c src/code.c src/debug.c src/decode.c src/long_term.c src/lpc.c src/preprocess.c src/rpe.c src/gsm_destroy.c src/gsm_decode.c src/gsm_encode.c src/gsm_explode.c src/gsm_implode.c src/gsm_create.c src/gsm_print.c src/gsm_option.c src/short_term.c src/table.c)
build_case() {
 local flags=(-std=c99 "-O${GSM_OPT:-2}" -Wno-deprecated-non-prototype -Wno-comment -Iinc -DWAV49 -DNeedFunctionPrototypes=1)
 if [[ ${GSM_NO_SASR:-0} = 0 ]]; then flags+=(-DSASR); fi
 if [[ ${GSM_TARGET:-0} = 1 ]]; then flags+=(-fsanitize=undefined -fno-sanitize-recover=undefined); fi
 cc "${flags[@]}" "${lib_sources[@]}" "$@" -o "$b/case"
}
