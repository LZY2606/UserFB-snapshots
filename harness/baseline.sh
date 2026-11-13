#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
bash harness/build.sh baseline
java -ea -cp .build/classes:src/test/resources LegacyCases
java -ea -cp .build/classes:src/test/resources StartingPoint
