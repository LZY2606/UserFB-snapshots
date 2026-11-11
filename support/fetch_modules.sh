#!/usr/bin/env bash
# Fetches modules listed in go.sum from proxy.golang.org using curl
# (works behind TLS-intercepting proxies that Go cannot verify) and
# stores them as a local file GOPROXY under .build/goproxy.
set -euo pipefail
cd "$(dirname "$0")/.."
OUT="$PWD/.build/goproxy"
mkdir -p "$OUT"

enc() { # GOPROXY module path encoding: uppercase -> !lowercase
  echo "$1" | awk '{for(i=1;i<=length($0);i++){c=substr($0,i,1);if(c>="A"&&c<="Z")printf "!" tolower(c);else printf c}printf "\n"}'
}

awk '{print $1" "$2}' go.sum | sed 's|/go\.mod$||' | sort -u | while read -r mod ver; do
  emod=$(enc "$mod")
  dir="$OUT/$emod/@v"
  mkdir -p "$dir"
  base="https://proxy.golang.org/$emod/@v/$ver"
  for ext in info mod zip; do
    [ -s "$dir/$ver.$ext" ] || curl -sfL --retry 3 -o "$dir/$ver.$ext" "$base.$ext" || {
      echo "FAILED $base.$ext" >&2; exit 1; }
  done
done
echo "module proxy ready at $OUT"
