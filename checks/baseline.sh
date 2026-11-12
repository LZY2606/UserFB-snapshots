#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
test -x .cff2-check/baseline || { echo 'run sh checks/install.sh first' >&2; exit 2; }
exec .cff2-check/baseline
