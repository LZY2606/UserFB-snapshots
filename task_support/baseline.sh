#!/bin/bash
set -euo pipefail
.task-build/baseline
.task-build/ogg/test_bitwise
.task-build/ogg/test_framing
printf 'OGG_SELFTESTS_COMPLETE\n'
