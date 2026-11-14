#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
source task_support/env.sh
go build "${production[@]}"
echo 'INSTALL standard-library production build OK'
