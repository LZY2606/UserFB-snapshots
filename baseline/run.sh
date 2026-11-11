set -euo pipefail
b=$(mktemp -d)
trap 'rm -rf "$b"' EXIT
clang -g -O1 -fsanitize=address,undefined -fno-omit-frame-pointer -Isrc baseline/witness.c src/microtar.c -o "$b/witness"
"$b/witness"
set +e
ASAN_OPTIONS=detect_leaks=0 "$b/witness" malformed >"$b/crash.log" 2>&1
rc=$?
set -e
test "$rc" -ne 0
grep -q 'AddressSanitizer: stack-buffer-overflow' "$b/crash.log"
cat "$b/crash.log"
printf '%s\n' 'MICROTAR malformed fixed field reproduces stack-buffer-overflow'
