set -euo pipefail
mode=${1:-baseline}
b=$(mktemp -d)
trap 'rm -rf "$b"' EXIT
cmake -S . -B "$b" -DCMAKE_POLICY_VERSION_MINIMUM=3.5 -DRE2_BUILD_TESTING=ON -DUSEPCRE=OFF -DBUILD_SHARED_LIBS=OFF >"$b/config.log" 2>&1
cmake --build "$b" --target re2 re2_test dfa_test -j 4 >"$b/build.log" 2>&1 || { cat "$b/build.log"; exit 1; }
if [ "$mode" = baseline ]; then
  c++ -std=c++11 -I. checks/baseline.cc "$b/libre2.a" -pthread -o "$b/case"
else
  cc -Ichecks -c checks/harness.c -o "$b/harness.o"
  c++ -std=c++11 -I. checks/target.cc "$b/harness.o" "$b/libre2.a" -pthread -o "$b/case"
fi
"$b/case" >"$b/out"
cat "$b/out"
if [ "$mode" != baseline ]; then bash checks/check-output.sh "$b/out"; fi
"$b/re2_test" >"$b/old.log" 2>&1
"$b/dfa_test" >>"$b/old.log" 2>&1
printf '%s\n' RE2_OLD_TESTS_OK
