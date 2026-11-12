set -euo pipefail
mkdir -p "$1"
for file in alloc net hiredis sds async read sockcompat; do
 cc -std=c99 -O1 -DHIREDIS_FLOAT_STRTOD -I. -c "$file.c" -o "$1/$file.o"
done
ar rcs "$1/libhiredis.a" "$1"/*.o
