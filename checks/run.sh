set -euo pipefail
build=$(mktemp -d "${TMPDIR:-/tmp}/ofbx-check.XXXXXX")
trap 'rm -rf "$build"' EXIT
cc -O1 -c src/libdeflate.c -o "$build/deflate.o"
mode=${1:-baseline}
if [ "$mode" = baseline ];then entry=checks/baseline.cpp;extras=(src/ofbx.cpp);elif [ "$mode" = target ];then entry=checks/registered.cpp;extras=(src/ofbx.cpp src/ofbx_pose.cpp);else exit 2;fi
c++ -std=c++17 -O1 -Isrc "${extras[@]}" "$entry" "$build/deflate.o" -o "$build/run"
"$build/run"
