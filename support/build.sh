set -euo pipefail
cmake -S . -B "$1" -DCMAKE_POLICY_VERSION_MINIMUM=3.5 -DBUILD_SHARED_LIBS=OFF -DBUILD_TESTING=OFF -DBUILD_EXAMPLES=OFF -DWITH_ZLIB=OFF >/dev/null
cmake --build "$1" --target tag -j4 >/dev/null
