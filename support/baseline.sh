set -euo pipefail
build=$(mktemp -d)
trap 'rm -rf "$build"' EXIT
bash support/build.sh support/baseline.cpp "$build/baseline"
"$build/baseline"
