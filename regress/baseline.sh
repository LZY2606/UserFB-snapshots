set -euo pipefail
cd "$(dirname "$0")/.."
bash regress/legacy.sh
build=$(mktemp -d)
trap 'rm -rf "$build"' EXIT
c++ -std=c++11 -O1 -I. source/libMC33++.cpp regress/oblique_witness.cpp -o "$build/observe"
"$build/observe" --expect-defect
