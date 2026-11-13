set -euo pipefail
build_task() { local src=$1 out=$2; shift 2; "${CXX:-c++}" -std=c++17 -O2 -I. -Itask_support "$src" "$@" -o "$out"; }
