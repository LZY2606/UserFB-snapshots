set -euo pipefail
b=$(mktemp -d)
trap 'rm -rf "$b"' EXIT
files=(); for f in src/*.cpp; do [[ "$f" == src/Main.cpp ]] || files+=("$f"); done
c++ -std=c++11 -I src "${files[@]}" checks/target.cpp checks/target_main.c -o "$b/check"
"$b/check" | tee "$b/target.log"
for group in flow calls compat; do
 grep -Eq "^GROUP_OK $group [1-9][0-9]*$" "$b/target.log"
done
grep -Eq '^TARGET_OK [1-9][0-9]*$' "$b/target.log"
bash checks/baseline.sh
