#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
bash harness/runtime.sh harness/baseline.mjs
bash harness/runtime.sh harness/run-tests.mjs old
