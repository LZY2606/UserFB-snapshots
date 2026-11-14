#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mkdir -p .task-build/obj
printf '#define WEBRTC_POSIX 1\n#define PACKAGE_VERSION "2.0.2"\n' >.task-build/config.h
sources=(ilbc/*.c)
for src in signal_processing/*.c; do
  case "$src" in *_mips.c|*_neon.c) continue;; esac
  sources+=("$src")
done
objects=()
for src in "${sources[@]}"; do
  obj=".task-build/obj/${src//\//_}.o"
  if [[ ! -f "$obj" || "$src" -nt "$obj" ]]; then
    "${CC:-cc}" -std=c99 -O2 -fwrapv -fno-strict-aliasing -DWEBRTC_POSIX -I. -Iilbc/interface -Isignal_processing/include -I.task-build -c "$src" -o "$obj"
  fi
  objects+=("$obj")
done
"${AR:-ar}" rcs .task-build/libilbc.a "${objects[@]}"
