set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
cd "$root"
build=$(mktemp -d)
trap 'rm -rf "$build"' EXIT
source harness/build.sh
build_baseline "$build/baseline"
"$build/baseline"
cc -std=c99 -O1 -I. harness/geometry_witness.c -lm -o "$build/geometry"
"$build/geometry"
cc -std=c99 -O1 example/voxelizer.c -lm -o "$build/example"
"$build/example"
printf 'UPSTREAM_C_EXAMPLE_OK\n'
