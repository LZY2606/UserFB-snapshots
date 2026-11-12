set -euo pipefail
build=$(mktemp -d)
trap 'rm -rf "$build"' EXIT
cc -std=c99 -O1 -DHTTP_PARSER_STRICT=1 -I. http_parser.c baseline/witness.c -o "$build/witness"
"$build/witness"
cc -std=c99 -O1 -DHTTP_PARSER_STRICT=1 -I. http_parser.c test.c -o "$build/old"
"$build/old"
