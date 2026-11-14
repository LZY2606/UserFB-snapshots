#!/bin/bash
set -euo pipefail
bash task_support/go.sh test -vet=off -mod=vendor -run '^$' .
