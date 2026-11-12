set -euo pipefail
build=$(mktemp -d)
trap 'rm -rf "$build"' EXIT
bash support/build.sh "$build"
bash support/link.sh "$build" support/baseline.cpp "$build/baseline"
"$build/baseline"
