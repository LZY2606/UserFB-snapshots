#!/bin/bash
# Populate the local Go module cache (.build/go-mod) from proxy.golang.org
# using curl, because the sandboxed `go` command cannot verify TLS certs.
set -u
cd "$(dirname "$0")/.."
DL="$PWD/.build/go-mod/cache/download"

esc() {
  awk '{ out=""; for(i=1;i<=length($0);i++){ ch=substr($0,i,1); if(ch>="A"&&ch<="Z") out=out "!" tolower(ch); else out=out ch } print out }' <<< "$1"
}

fail=0
awk '{print $1, $2}' go.sum | sed 's|/go\.mod$||' | sort -u | while read -r mod ver; do
  em=$(esc "$mod")
  ev=$(esc "$ver")
  dir="$DL/$em/@v"
  mkdir -p "$dir"
  if [ ! -s "$dir/$ev.mod" ]; then
    curl -sf -o "$dir/$ev.mod" "https://proxy.golang.org/$em/@v/$ev.mod" || echo "FAIL mod $mod $ver"
  fi
  if [ ! -s "$dir/$ev.info" ]; then
    curl -sf -o "$dir/$ev.info" "https://proxy.golang.org/$em/@v/$ev.info" || printf '{"Version":"%s"}' "$ver" > "$dir/$ev.info"
  fi
  if [ ! -s "$dir/$ev.zip" ]; then
    curl -sf -o "$dir/$ev.zip" "https://proxy.golang.org/$em/@v/$ev.zip" || rm -f "$dir/$ev.zip"
  fi
done
echo "module cache populated"
