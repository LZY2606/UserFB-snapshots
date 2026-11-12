set -euo pipefail
build=$(mktemp -d)
trap 'rm -rf "$build"' EXIT
c++ -std=c++11 -O1 -I. support/baseline.cpp -o "$build/baseline"
"$build/baseline"
cc -O1 minimp4_test.c -o "$build/old"
for mode in normal sequential fragmented slices; do
 case "$mode" in
 normal) "$build/old" vectors/foreman.264 "$build/out.mp4"; ref=vectors/out_ref.mp4;;
 sequential) "$build/old" -s vectors/foreman.264 "$build/out.mp4"; ref=vectors/out_sequential_ref.mp4;;
 fragmented) "$build/old" -f vectors/foreman.264 "$build/out.mp4"; ref=vectors/out_fragmentation_ref.mp4;;
 slices) "$build/old" vectors/foreman_slices.264 "$build/out.mp4"; ref=vectors/out_slices_ref.mp4;;
 esac
 cmp "$build/out.mp4" "$ref"
done
printf 'OLD_MUX_BYTES_OK 4\n'
