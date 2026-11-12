set -euo pipefail
mkdir -p "$1"
cc -std=c99 -O1 -DMAKE_LIB -I. -c onelua.c -o "$1/lua.o"
