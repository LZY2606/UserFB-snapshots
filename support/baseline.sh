set -euo pipefail
build=$(mktemp -d)
trap 'rm -rf "$build"' EXIT
c++ -std=c++11 -O1 -Iinclude src/*.cpp support/baseline.cpp -o "$build/baseline"
"$build/baseline"
