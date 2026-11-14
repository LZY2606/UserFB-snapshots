set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
cd "$root"
build=$(mktemp -d)
trap 'rm -rf "$build"' EXIT
source harness/build.sh
build_target "$build/target"
"$build/target"
bash harness/baseline.sh
