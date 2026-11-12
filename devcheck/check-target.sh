#!/usr/bin/env bash
set -euo pipefail
node devcheck/run-target.mjs
bash devcheck/baseline.sh
