set -euo pipefail
build=$(mktemp -d);trap 'rm -rf "$build"' EXIT
bash checks/build.sh "$build"
c++ -std=c++11 -I include checks/harness.cpp tests/ipv6_pseudoheader_test.cpp "$build/lib/libtins.a" -o "$build/target"
"$build/target"
c++ -std=c++11 -I include checks/control.cpp "$build/lib/libtins.a" -o "$build/control"
"$build/control"
echo IPV6_PSEUDOHEADER_TARGET_OK
