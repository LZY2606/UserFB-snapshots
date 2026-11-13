set -euo pipefail
cd "$(dirname "$0")/.."
build=$(mktemp -d)
trap 'rm -rf "$build"' EXIT
source task_support/build.sh
build_task task_support/baseline.cpp "$build/baseline"
"$build/baseline"
