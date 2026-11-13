#!/bin/sh
set -eu
mkdir -p .build
c++ -std=c++17 -O1 -pthread -I. -Isupport -Iexts/stb_image -Iexts/fast_float/include "$@" libs/yocto/yocto_trace.cpp libs/yocto/yocto_scene.cpp libs/yocto/yocto_bvh.cpp libs/yocto/yocto_shape.cpp libs/yocto/yocto_image.cpp libs/yocto/yocto_modelio.cpp exts/stb_image/stb_image/stb_image.cpp -o .build/check
