set -euo pipefail
build=$(mktemp -d);trap 'rm -rf "$build"' EXIT
bash checks/build.sh "$build"
cc -std=c99 -I include -I crypto/include checks/baseline.c "$build/libsrtp3.a" -o "$build/witness"
"$build/witness"
ctest --test-dir "$build" -R '^(rdbx_driver|replay_driver|roc_driver|srtp_driver)$' --output-on-failure
echo SRTP_OLD_TESTS_OK
