set -euo pipefail
build=$(mktemp -d);trap 'rm -rf "$build"' EXIT
bash checks/build.sh "$build"
cc -std=c99 -I include -I crypto/include checks/harness.c test/replay_update_test.c "$build/libsrtp3.a" -o "$build/target"
"$build/target"
ctest --test-dir "$build" -R '^(rdbx_driver|replay_driver|roc_driver|srtp_driver)$' --output-on-failure
echo SRTP_TARGET_OK
