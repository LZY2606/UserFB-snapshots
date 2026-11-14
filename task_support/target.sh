#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
exec bash task_support/run.sh target
