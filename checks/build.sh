set -euo pipefail
build="$(mktemp -d)"
trap 'rm -r "$build"' EXIT
c++ -O2 -std=c++11 -DNDEBUG -Ishared/DSPFilters/include checks/baseline.cpp shared/DSPFilters/source/{Butterworth,Biquad,Cascade,PoleFilter}.cpp -o "$build/baseline"
"$build/baseline"
