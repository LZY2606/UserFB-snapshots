#!/usr/bin/env bash
set -euo pipefail
out=$1
cc=${CC:-clang}
flags=(-std=gnu11 -O1 -D_GNU_SOURCE -DHAVE_STDBOOL_H=1 -DBUCKETS_128 -DCHECKSUM_1B -DUSE_NO_PROC -I libyara/include -I libyara -pthread)
if [[ $(uname -s) == Darwin ]]; then flags+=(-DHAVE_STRLCPY=1 -DHAVE_STRLCAT=1 -DHAVE_MEMMEM=1); fi
sources=(libyara/*.c libyara/proc/none.c libyara/tlshc/*.c libyara/modules/tests/tests.c libyara/modules/elf/elf.c libyara/modules/math/math.c libyara/modules/time/time.c libyara/modules/pe/pe.c libyara/modules/pe/pe_utils.c libyara/modules/console/console.c libyara/modules/string/string.c)
objects=()
for src in "${sources[@]}"; do
 obj="$out/$(printf '%s' "$src" | tr '/' '_').o"
 "$cc" "${flags[@]}" -c "$src" -o "$obj"
 objects+=("$obj")
done
ar rcs "$out/libyara.a" "${objects[@]}"
