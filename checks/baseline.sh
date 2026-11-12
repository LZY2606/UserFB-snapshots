set -euo pipefail
build=$(mktemp -d);trap 'rm -rf "$build"' EXIT
bash checks/build.sh "$build"
c++ -std=c++11 -I pe-parser-library/include checks/baseline.cpp "$build/pe-parser-library/libpe-parse.a" -o "$build/witness"
"$build/witness"
