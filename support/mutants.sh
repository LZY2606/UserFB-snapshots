set -euo pipefail
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
mkdir "$work/src"
tar -cf - --exclude=.git . | (cd "$work/src" && tar -xf -)
cd "$work/src"
bash support/build.sh "$work/build"
for name in tag23 frame24 dli; do
 # GNU Make 3.81 (macOS) compares mtimes at whole-second resolution.  Let the
 # patched source land in a later second than the previous build's objects,
 # otherwise the mutated translation unit or archive update can be skipped.
 sleep 2
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
 sleep 2
 cmake --build "$work/build" --target tag -j4 >/dev/null
done
