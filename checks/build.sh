set -euo pipefail
build="$(mktemp -d)"
trap 'rm -r "$build"' EXIT
c++ -O2 -std=c++11 checks/prepare.cpp -o "$build/prepare"
"$build/prepare" "$build"
c++ -O2 -std=c++11 -DVERSION='"pinned"' -I"$build" -I. checks/baseline.cpp sid.cc voice.cc wave.cc envelope.cc filter.cc dac.cc extfilt.cc pot.cc -o "$build/baseline"
"$build/baseline"
