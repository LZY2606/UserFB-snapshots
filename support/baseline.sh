set -euo pipefail
build=$(mktemp -d 2>/dev/null || mktemp -d "${TMPDIR:-/tmp}/zimg-baseline.XXXXXX")
trap 'rm -rf "$build"' EXIT
bash support/build.sh support/baseline.cpp "$build/baseline"
"$build/baseline"
