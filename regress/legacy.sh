set -euo pipefail
cd "$(dirname "$0")/.."
build=$(mktemp -d)
trap 'rm -rf "$build"' EXIT
source regress/build.sh
build_baseline "$build/baseline"
"$build/baseline"
