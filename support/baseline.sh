set -euo pipefail
build=$(mktemp -d)
trap 'rm -rf "$build"' EXIT
bash support/build.sh "$build" baseline
"$build/baseline"
cmake --build "$build" --target chdr-kat -j4 >/dev/null
"$build/tests/chdr-kat"
echo CHDR_OLD_KAT_OK
