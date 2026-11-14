#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mkdir -p .task-build
variant=${1:?float or fixed}
extra=(-DSNAPSHOT_BUILD=1)
[[ $variant == fixed ]] && extra=(-DFIXED_POINT)
flags=(-std=c99 -O1 -include math.h -I. -Iinclude -Ilibfaad -DHAVE_INTTYPES_H=1 -DHAVE_MEMCPY=1 -DHAVE_STRING_H=1 -DHAVE_STRINGS_H=1 -DHAVE_SYS_STAT_H=1 -DHAVE_SYS_TYPES_H=1 -DHAVE_LRINTF=1 -DAPPLY_DRC '-DPACKAGE_VERSION="task-snapshot"')
if [[ ${2:-baseline} == baseline ]]; then
 cc "${flags[@]}" "${extra[@]}" task_support/baseline.c libfaad/drc.c libfaad/common.c -lm -o ".task-build/baseline-$variant"
else
 test -s tests/drc_contract.c
 cc "${flags[@]}" "${extra[@]}" -DDRC_CONTRACT_TEST task_support/driver.c tests/drc_contract.c libfaad/*.c -lm -o ".task-build/target-$variant"
fi
