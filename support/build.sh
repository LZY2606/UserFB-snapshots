#!/bin/sh
set -eu
cmake -S . -B .build -DBUILD_APPS=OFF -DCMAKE_BUILD_TYPE=Release -DCMAKE_OSX_ARCHITECTURES="$(uname -m)"
cmake --build .build --target ap4 -j 4
