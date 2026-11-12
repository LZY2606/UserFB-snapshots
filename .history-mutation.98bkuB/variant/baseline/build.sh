set -euo pipefail
cmake -S . -B "$1" -DBUILD_SHARED_LIBS=OFF -DBUILD_TEST=ON -DCMAKE_BUILD_TYPE=Release
cmake --build "$1" --target onig test_syntax -j 2
