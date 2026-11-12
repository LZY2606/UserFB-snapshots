set -euo pipefail
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
mkdir "$work/src"
tar -cf - --exclude=.git . | (cd "$work/src" && tar -xf -)
cd "$work/src"
bash support/build.sh "$work/build"
for name in tag23 frame24 dli; do
 patch -p1 < "support/mutant-$name.patch" >/dev/null
 cmake --build "$work/build" --target tag -j4 >/dev/null
 bash support/link.sh "$work/build" support/target_main.cpp "$work/test"
 set +e
 "$work/test" > "$work/output" 2>&1
 code=$?
 set -e
 if [ "$code" -ne 1 ] || ! grep -q '^FAIL .*target_cases.cpp:' "$work/output"; then
   cat "$work/output"
   echo "MUTANT_NOT_SEMANTICALLY_REJECTED $name exit=$code" >&2
   exit 1
 fi
 echo "MUTANT_REJECTED $name build=0 test=1"
 patch -R -p1 < "support/mutant-$name.patch" >/dev/null
 cmake --build "$work/build" --target tag -j4 >/dev/null
done
