set -euo pipefail
build=$(mktemp -d)
trap 'rm -rf "$build"' EXIT
export GOPROXY=off GOSUMDB=off GOTOOLCHAIN=local
export GOCACHE=${GOCACHE:-$build/cache}
export GOMODCACHE=${GOMODCACHE:-$build/mod}
go test ./ip6defrag -count=1 -v -run '^TestRawIPv6Contract$' | tee "$build/result"
for name in TestRawIPv6Contract TestRawIPv6Contract/extensions TestRawIPv6Contract/ecn TestRawIPv6Contract/atomic TestRawIPv6Contract/identity; do
 awk -v name="$name" '$1=="---" && $2=="PASS:" && $3==name {n++} END {exit(n!=1)}' "$build/result"
done
bash baseline/run.sh
printf 'RAW_IPV6_CONTRACT_OK\n'
