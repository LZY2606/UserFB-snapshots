set -euo pipefail
build=$(mktemp -d "${TMPDIR:-/tmp}/dragonbones-check.XXXXXX")
trap 'rm -rf "$build"' EXIT
sources=()
while IFS= read -r f;do sources+=("$f");done < <(find DragonBones/src -name '*.cpp' | sort)
mode=${1:-baseline}
if [ "$mode" = baseline ];then entry=checks/baseline.cpp;elif [ "$mode" = target ];then entry=checks/registered.cpp;else exit 2;fi
c++ -std=c++17 -O1 -w -include typeinfo -IDragonBones/src -I3rdParty "${sources[@]}" "$entry" -o "$build/run"
"$build/run"
