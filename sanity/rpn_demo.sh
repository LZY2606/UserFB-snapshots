#!/usr/bin/env bash
set -euo pipefail
build=$(mktemp -d "${TMPDIR:-/tmp}/stk-rpn.XXXXXX")
trap 'rm -rf "$build"' EXIT
c++ -std=c++11 -D_STK_DEBUG_ -Iinclude -Isanity sanity/rpn_demo.cpp src/Stk.cpp src/SineWave.cpp src/Voicer.cpp src/MidiChannelVoicer.cpp -o "$build/rpn_demo"
"$build/rpn_demo"
