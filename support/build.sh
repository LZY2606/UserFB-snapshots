#!/bin/bash
set -euo pipefail
cc -std=c99 -O2 -DCRYPTO_EDDSA_BATCH_TEST -Isrc -Isupport src/monocypher.c "$@"
