set -euo pipefail
build=$(mktemp -d);trap 'rm -rf "$build"' EXIT
bash checks/build.sh "$build"
c++ -std=c++11 -I include checks/baseline.cpp "$build/lib/libtins.a" -o "$build/witness"
"$build/witness"
c++ -std=c++11 -I include checks/control.cpp "$build/lib/libtins.a" -o "$build/control"
"$build/control"
