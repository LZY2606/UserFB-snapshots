set -euo pipefail
build="$(mktemp -d)"
trap 'rm -r "$build"' EXIT
c++ -O2 -std=c++11 -I. checks/target.cpp -o "$build/target"
for group in equivalence schedule boundaries realtime; do
  "$build/target" "$group" > "$build/out"
  cat "$build/out"
  awk -v g="$group" '$1=="PASS" && $2==g && $3 ~ /^checks=[1-9][0-9]*$/ {n++} END {exit n!=1}' "$build/out"
done
bash checks/build.sh
