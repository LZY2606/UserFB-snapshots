set -euo pipefail
build="$(mktemp -d)"
trap 'rm -r "$build"' EXIT
c++ -O2 -std=c++11 -I. checks/baseline.cpp WDL/resample.cpp -o "$build/baseline"
"$build/baseline"
