#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
command -v cc >/dev/null
command -v c++ >/dev/null
command -v node >/dev/null
node --input-type=module -e 'if(Number(process.versions.node.split(".")[0])<18)process.exit(1)'
node colrcheck/make-font.mjs --check
printf 'COLR_TOOLCHAIN_OK dependencies=none\n'
