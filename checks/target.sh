set -euo pipefail
build="$(mktemp -d)"
trap 'rm -r "$build"' EXIT
c++ -O2 -std=c++11 -DNDEBUG -Ishared/DSPFilters/include checks/target.cpp shared/DSPFilters/source/{Butterworth,Biquad,Cascade,PoleFilter}.cpp -o "$build/target"
for group in mapping pcm; do
  "$build/target" "$group" > "$build/out"
  cat "$build/out"
  awk -v g="$group" '$1=="PASS" && $2==g && $3 ~ /^checks=[1-9][0-9]*$/ {n++} END {exit n!=1}' "$build/out"
done
bash checks/build.sh
bash checks/mutations.sh > "$build/mutations"
cat "$build/mutations"
for name in prewarp normalization; do
  awk -v g="$name" '$1=="DETECTED" && $2==g && $3=="build=0" && $4=="numeric=1" {n++} END {exit n!=1}' "$build/mutations"
done
