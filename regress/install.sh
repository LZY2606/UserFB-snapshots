#!/usr/bin/env bash
set -euo pipefail
bash regress/go.sh mod download
bash regress/go.sh mod verify
docker pull --platform linux/arm64 chromedp/headless-shell@sha256:d0c5b37a80f6180f28b2824d8844bf0c10bdf54ebaaf1f97514b66e5a20bef4f
docker run --rm --platform linux/arm64 --entrypoint /headless-shell/headless-shell chromedp/headless-shell@sha256:d0c5b37a80f6180f28b2824d8844bf0c10bdf54ebaaf1f97514b66e5a20bef4f --version
