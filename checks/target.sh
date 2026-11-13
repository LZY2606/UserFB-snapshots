set -euo pipefail
build="$(mktemp -d)"
trap 'rm -r "$build"' EXIT
c++ -O2 -std=c++11 checks/prepare.cpp -o "$build/prepare"
"$build/prepare" "$build"
c++ -O2 -std=c++11 -DVERSION='"pinned"' -I"$build" -I. checks/target.cpp sid.cc voice.cc wave.cc envelope.cc filter.cc dac.cc extfilt.cc pot.cc -o "$build/target"
for group in digital edges mixed skip; do
  "$build/target" "$group" > "$build/out"
  cat "$build/out"
  awk -v g="$group" '$1=="PASS" && $2==g && $3 ~ /^checks=[1-9][0-9]*$/ {n++} END {exit n!=1}' "$build/out"
done
bash checks/build.sh
