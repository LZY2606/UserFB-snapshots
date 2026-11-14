#!/bin/bash
set -euo pipefail
mkdir -p .task-build/obj
cat > .task-build/config.h <<'CONFIG'
#ifndef TASK_CONFIG_H
#define TASK_CONFIG_H
#define HAVE_LIBPOPT 0
#define HAVE_GCC 1
#define HAVE_STRDUP 1
#define HAVE_STRERROR 1
#define STDC_HEADERS 1
#if defined(__APPLE__)
#include <machine/endian.h>
#elif defined(__linux__)
#include <endian.h>
#endif
#ifndef BYTE_ORDER
#define BYTE_ORDER __BYTE_ORDER__
#endif
#ifndef BIG_ENDIAN
#define BIG_ENDIAN __ORDER_BIG_ENDIAN__
#endif
#ifndef LITTLE_ENDIAN
#define LITTLE_ENDIAN __ORDER_LITTLE_ENDIAN__
#endif
#endif
CONFIG
# Scalar production code; no architecture assembler or optional popt/display stack.
units=(dv dct idct_248 weighting quant vlc place parse bitstream YUY2 YV12 rgb audio util encode headers enc_input enc_audio_input enc_output)
# Additional implementation files may be added without placing answers in this seed.
for extra in libdv/*stream*.c; do
  if [[ -f "$extra" ]]; then units+=("$(basename "$extra" .c)"); fi
done
for unit in "${units[@]}"; do
  cc -std=gnu99 -O2 -DHAVE_CONFIG_H=1 -I.task-build -I. -Ilibdv -c "libdv/$unit.c" -o ".task-build/obj/$unit.o"
done
ar rcs .task-build/libdv.a .task-build/obj/*.o
