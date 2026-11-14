#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
command -v go >/dev/null
go version
