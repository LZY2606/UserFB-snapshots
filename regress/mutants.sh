set -euo pipefail
cd "$(dirname "$0")/.."
build=$(mktemp -d)
trap 'rm -rf "$build"' EXIT
for kind in face root; do
  dir="$build/$kind"; mkdir -p "$dir"; cp -R source include "$dir/"
  case "$kind" in
    face) sed 's/\(v\[[0-9]\]\*v\[[0-9]\]\) < \(v\[[0-9]\]\*v\[[0-9]\]\)/\1 > \2/g' source/MC33.cpp > "$dir/source/MC33.cpp"; group=ambiguous;;
    root) sed 's/r\[0\] = x + t;/r[0] = x + t*0.75f;/g' source/MC33.cpp > "$dir/source/MC33.cpp"; group=analytic;;
  esac
  # A renamed/removed mutation site is an error, never an automatic detection.
  if cmp -s source/MC33.cpp "$dir/source/MC33.cpp"; then echo "mutation site missing: $kind" >&2; exit 2; fi
  MC_SOURCE_ROOT="$dir"; source regress/build.sh
  build_target "$dir/target"
  set +e
  "$dir/target" > "$dir/output.log" 2>&1
  rc=$?
  set -e
  cat "$dir/output.log"
  test "$rc" -eq 1
  grep -Eq '^GENERATED .*vertices=[1-9][0-9]*.*triangles=[1-9][0-9]*' "$dir/output.log"
  grep -q "FAIL $group count=-" "$dir/output.log"
  echo "DETECTED $kind"
done
