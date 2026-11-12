#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
bash harness/runtime.sh --max-old-space-size=128 harness/run-tests.mjs target
bash harness/runtime.sh harness/run-tests.mjs old
bash harness/runtime.sh harness/baseline.mjs
