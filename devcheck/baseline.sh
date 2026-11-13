set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
cd "$root"
build=$(mktemp -d)
trap 'rm -rf "$build"' EXIT
source devcheck/build.sh
build_baseline "$build/baseline"
"$build/baseline"
