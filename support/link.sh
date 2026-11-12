set -euo pipefail
includes=()
while IFS= read -r p; do includes+=("-I$p"); done < <(find taglib -type d | LC_ALL=C sort)
c++ -std=c++11 -O1 -I"$1" -Isupport "${includes[@]}" "$2" "$1/taglib/libtag.a" -o "$3"
