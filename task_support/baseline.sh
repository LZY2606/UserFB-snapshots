#!/bin/bash
set -euo pipefail
bash task_support/go.sh test -mod=vendor -count=1 -timeout=30s -v -run '^(Test_ParseJSSource|TestGetExtType)$' ./core
bash task_support/go.sh run -mod=vendor ./task_support/witness
