#!/usr/bin/env bash
set -euo pipefail
node --import ./devcheck/offline-loader.mjs devcheck/baseline.mjs
node --import ./devcheck/offline-loader.mjs --test --test-reporter=tap test/base64-decoder-test.ts test/qp-decoder-test.ts test/pass-through-decoder-test.ts
