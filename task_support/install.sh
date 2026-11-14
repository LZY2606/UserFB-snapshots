#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
command -v go >/dev/null
test -s vendor/modules.txt
go version
