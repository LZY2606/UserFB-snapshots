#!/bin/bash
set -euo pipefail
bash task_support/go.sh test -mod=vendor -run '^$' ./core
printf '%s\n' VENDORED_DEPENDENCIES_READY
