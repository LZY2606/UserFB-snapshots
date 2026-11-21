#!/bin/bash
set -euo pipefail
test -s devcheck/legacy-hash.txt
.task-build/baseline > .task-build/baseline.out
cat .task-build/baseline.out
grep '^LEGACY_BYTES_FNV=' .task-build/baseline.out | cmp - devcheck/legacy-hash.txt
