set -euo pipefail
cd "$(dirname "$0")/.."
build=$(mktemp -d)
trap 'rm -rf "$build"' EXIT
source task_support/build.sh
build_library "$build"
"${CC:-cc}" -std=c11 -O2 -Iinclude task_support/baseline.c "$build"/*.o -lm -o "$build/baseline"
"$build/baseline"
