set -euo pipefail
common=(-std=c++11 -O1 -I. "${MC_SOURCE_ROOT:-.}/source/libMC33++.cpp")
build_baseline() { c++ "${common[@]}" regress/baseline.cpp -o "$1"; }
build_target() { c++ "${common[@]}" regress/driver.cpp regress/regression.cpp -o "$1"; }
