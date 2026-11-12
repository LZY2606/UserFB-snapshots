set -euo pipefail
mode=${1:-baseline}
b=$(mktemp -d)
trap 'rm -rf "$b"' EXIT
cmake -S . -B "$b" -DASMJIT_STATIC=ON -DASMJIT_TEST=OFF -DASMJIT_NO_X86=ON -DASMJIT_NO_AARCH64=ON -DASMJIT_NO_COMPILER=ON -DASMJIT_NO_UJIT=ON -DASMJIT_NO_BUILDER=ON >"$b/config.log" 2>&1
cmake --build "$b" --target asmjit -j 4 >"$b/build.log" 2>&1 || { cat "$b/build.log"; exit 1; }
if [ "$mode" = baseline ]; then
 c++ -std=c++20 -DASMJIT_STATIC -I. harness/baseline.cc "$b/libasmjit.a" -pthread -o "$b/case"
else
 cc -Iharness -c harness/harness.c -o "$b/harness.o"
 c++ -std=c++20 -DASMJIT_STATIC -I. harness/target.cc "$b/harness.o" "$b/libasmjit.a" -pthread -o "$b/case"
fi
"$b/case" >"$b/out"
cat "$b/out"
if [ "$mode" != baseline ]; then bash harness/check-output.sh "$b/out"; fi

c++ -std=c++20 -DASMJIT_STATIC -I. harness/control.cc "$b/libasmjit.a" -pthread -o "$b/control"
"$b/control"
