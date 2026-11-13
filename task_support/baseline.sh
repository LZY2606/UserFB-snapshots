set -euo pipefail
cd "$(dirname "$0")/.."
build=$(mktemp -d "${TMPDIR:-/tmp}/qmdsp-baseline.XXXXXX")
trap 'rm -rf "$build"' EXIT
source task_support/build.sh
build_task task_support/baseline.cpp "$build/baseline"
"$build/baseline"
