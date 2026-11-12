set -euo pipefail
build=$(mktemp -d "${TMPDIR:-/tmp}/assimp-weld.XXXXXX")
trap 'rm -rf "$build"' EXIT
cmake -S . -B "$build" -DASSIMP_BUILD_ALL_IMPORTERS_BY_DEFAULT=OFF -DASSIMP_NO_EXPORT=ON -DASSIMP_BUILD_TESTS=OFF -DASSIMP_BUILD_ASSIMP_TOOLS=OFF -DASSIMP_BUILD_SAMPLES=OFF -DASSIMP_BUILD_ZLIB=ON -DASSIMP_BUILD_DRACO=OFF -DBUILD_SHARED_LIBS=OFF -DASSIMP_INSTALL=OFF -DASSIMP_WARNINGS_AS_ERRORS=OFF '-DCMAKE_C_FLAGS=-include stdio.h' > "$build/configure.log" 2>&1 || { cat "$build/configure.log";exit 1; }
cmake --build "$build" --target assimp --parallel 6 > "$build/compile.log" 2>&1 || { cat "$build/compile.log";exit 1; }
mode=${1:-baseline}
if [ "$mode" = baseline ];then entry=harness/baseline.cpp;elif [ "$mode" = target ];then entry=harness/registered.cpp;else exit 2;fi
lib=$(find "$build" -name libassimp.a -type f -print -quit)
zlib=$(find "$build" -name libzlibstatic.a -type f -print -quit)
test -n "$lib" && test -n "$zlib"
c++ -std=c++17 -O1 -Iinclude -Icode -I"$build/include" "$entry" "$lib" "$zlib" -o "$build/run"
"$build/run"
