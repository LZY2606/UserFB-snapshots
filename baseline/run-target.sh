set -euo pipefail
build=$(mktemp -d)
trap 'rm -rf "$build"' EXIT
cc -std=c99 -O1 -DHTTP_PARSER_STRICT=1 -I. http_parser.c fuzzers/session_contract.c baseline/target_driver.c -o "$build/target"
"$build/target"
cc -std=c99 -O1 -DHTTP_PARSER_STRICT=1 -I. http_parser.c test.c -o "$build/old"
"$build/old"
