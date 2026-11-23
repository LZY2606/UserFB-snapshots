#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
root_dir=$PWD
mkdir -p .task-build/obj
(cd .task-build/obj
 for src in "$root_dir"/gme/*.cpp; do
  case $(basename "$src") in
   Blip_Buffer.cpp|Classic_Emu.cpp|Data_Reader.cpp|Effects_Buffer.cpp|Gme_File.cpp|M3u_Playlist.cpp|Multi_Buffer.cpp|Music_Emu.cpp|Gb_Apu.cpp|Gb_Cpu.cpp|Gb_Oscs.cpp|Gbs_Emu.cpp|gme.cpp|Gb_*.cpp|Gbs_*.cpp|Multi_*.cpp|Classic_*.cpp) c++ -std=c++11 -O2 -DGEN_TYPES_H -include "$root_dir/task_support/platform.h" -I"$root_dir/task_support" -I"$root_dir/gme" -c "$src";;
  esac
 done >../compile.log 2>&1
 ar rcs ../libtaskgme.a ./*.o)
