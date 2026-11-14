#!/bin/bash
set -euo pipefail
.task-build/upstream-decode
.task-build/baseline
echo BASELINE_OK
