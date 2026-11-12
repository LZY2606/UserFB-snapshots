set -euo pipefail
build=$(mktemp -d "${TMPDIR:-/tmp}/ozz-sparse.XXXXXX")
trap 'rm -rf "$build"' EXIT
cmake -S . -B "$build" -Dozz_build_tools=OFF -Dozz_build_fbx=OFF -Dozz_build_gltf=OFF -Dozz_build_data=OFF -Dozz_build_samples=OFF -Dozz_build_howtos=OFF -Dozz_build_tests=OFF -Dozz_build_postfix=OFF -Dozz_build_simd_ref=ON > "$build/configure.log" 2>&1 || { cat "$build/configure.log";exit 1; }
cmake --build "$build" --target ozz_animation_offline --parallel 6 > "$build/compile.log" 2>&1 || { cat "$build/compile.log";exit 1; }
mode=${1:-baseline}
if [ "$mode" = baseline ];then entry=devcheck/baseline.cpp;elif [ "$mode" = target ];then entry=devcheck/registered.cpp;else exit 2;fi
offline=$(find "$build" -name libozz_animation_offline.a -type f -print -quit)
runtime=$(find "$build" -name libozz_animation.a -type f -print -quit)
base=$(find "$build" -name libozz_base.a -type f -print -quit)
test -n "$offline" && test -n "$runtime" && test -n "$base"
c++ -std=c++17 -DOZZ_BUILD_SIMD_REF -O1 -Iinclude -Isrc "$entry" "$offline" "$runtime" "$base" -o "$build/run"
"$build/run"
