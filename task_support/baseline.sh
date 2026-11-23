#!/bin/bash
set -euo pipefail
rm -rf .task-build/benign-corpus
mkdir -p .task-build/benign-corpus
printf A > .task-build/benign-corpus/a
printf B > .task-build/benign-corpus/b
.task-build/benign -runs=8 -seed=1 -max_len=4 -reload=0 .task-build/benign-corpus > .task-build/benign.log 2>&1
cat .task-build/benign.log
grep -q BENIGN_A .task-build/benign.log
grep -q BENIGN_OTHER .task-build/benign.log
grep -q 'DONE' .task-build/benign.log
printf 'BENIGN_PRODUCTION_BASELINE_OK\n'
