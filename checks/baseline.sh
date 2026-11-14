set -euo pipefail
b=$(mktemp -d)
trap 'rm -rf "$b"' EXIT
files=(); for f in src/*.cpp; do [[ "$f" == src/Main.cpp ]] || files+=("$f"); done
c++ -std=c++11 -I src "${files[@]}" checks/baseline.cpp -o "$b/check"
"$b/check"
