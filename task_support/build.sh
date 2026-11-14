#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
root_dir=$PWD
mkdir -p .task-build/config .task-build/obj
if [[ ! -s .task-build/config/config.h ]]; then
  (cd .task-build/config; sh "$root_dir/configure" --disable-shared --disable-frontend --disable-nasm --disable-libmpg123 --without-libiconv-prefix >configure.log 2>&1)
fi
(cd .task-build/obj
 cc -std=c99 -O2 -DHAVE_CONFIG_H -I../config -I"$root_dir" -I"$root_dir/include" -I"$root_dir/libmp3lame" -I"$root_dir/mpglib" -c "$root_dir"/libmp3lame/*.c "$root_dir"/mpglib/*.c >../compile.log 2>&1
 ar rcs ../libtasklame.a ./*.o)
