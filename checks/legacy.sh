#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
build-filter/filter_baseline --existing
(cd build-filter && ./upstream/examples/svg2png ../checks/fixtures/legacy.svg 64x48)
test -s build-filter/legacy.svg.png
printf '%s\n' UPSTREAM_SVG2PNG_OK
