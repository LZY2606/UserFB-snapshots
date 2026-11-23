#!/bin/bash
set -euo pipefail
.task-build/cmake/DisassembleSimple
.task-build/legacy
printf 'BASELINE_OK\n'
