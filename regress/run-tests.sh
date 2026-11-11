#!/usr/bin/env bash
set -euo pipefail
image=chromedp/headless-shell@sha256:d0c5b37a80f6180f28b2824d8844bf0c10bdf54ebaaf1f97514b66e5a20bef4f
mode=${1:?baseline or target or old}
case "$mode" in
 baseline) pkg=./regress; names=(TestSourceBindingBaseline);;
 target) pkg=./regress; names=(TestSourceBindingOrigin TestSourceBindingLifetime TestSourceBindingIsolation);;
 old) pkg=.; names=(TestExposeFunc TestExposeFuncIframe TestExposeFuncSurvivesNavigation);;
 *) exit 2;;
esac
pattern=$(IFS='|'; echo "${names[*]}")
CGO_ENABLED=0 GOOS=linux GOARCH=arm64 bash regress/go.sh test -c -o ".build/$mode.test" "$pkg"
log=$(mktemp)
cid=$(docker create --platform linux/arm64 --shm-size=256m --entrypoint /tmp/check.test -w /tmp -e CHROMEDP_TEST_RUNNER=/headless-shell/headless-shell "$image" -test.v -test.count=1 -test.timeout=60s -test.run="^($pattern)$")
trap 'docker rm -f "$cid" >/dev/null; rm -f "$log"' EXIT
docker cp ".build/$mode.test" "$cid:/tmp/check.test"
docker cp testdata "$cid:/tmp/testdata"
docker start -a "$cid" | tee "$log"
status=$(docker wait "$cid");test "$status" = 0
for n in "${names[@]}"; do
 grep -qx "=== RUN   $n" "$log" || { echo "missing target test: $n"; exit 1; }
 grep -q -- "--- PASS: $n " "$log"
done
! grep -q -- '--- SKIP:' "$log"
echo "SOURCE_BINDING_${mode}_OK"
