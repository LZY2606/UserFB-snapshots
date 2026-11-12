set -euo pipefail
# Copy only the CMake entry point to add a witness target; sources stay untouched.
cmake -S . -B "$1" -DBUILD_SHARED_LIBS=OFF -DCHDR_WANT_TESTS=ON -DCHDR_FLAC_BACKEND=drflac -DWITH_SYSTEM_ZLIB=OFF -DWITH_SYSTEM_ZSTD=OFF -DCMAKE_PROJECT_chdr_INCLUDE="$PWD/support/targets.cmake" >/dev/null
cmake --build "$1" --target "$2" -j4 >/dev/null
