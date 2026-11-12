set -euo pipefail
cd "$(dirname "$0")/.."
build=$(mktemp -d)
trap 'rm -rf "$build"' EXIT
source regress/build.sh
build_target "$build/target"
"$build/target" | tee "$build/output"
for group in analytic ambiguous affine storage; do
  grep -Eq "^PASS $group [1-9][0-9]*$" "$build/output"
done
bash regress/legacy.sh
# This independent starting-point observation must also become correct.
c++ -std=c++11 -O1 -I. source/libMC33++.cpp regress/oblique_witness.cpp -o "$build/oblique"
"$build/oblique"
echo MC33_COORDINATE_REPAIR_OK
