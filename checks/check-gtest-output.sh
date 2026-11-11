#!/usr/bin/env bash
set -euo pipefail
# Require actual execution, a positive pass count and no disabled/skipped tests.
awk '
/\[==========\] Running [0-9]+ test/ { expected=$3 }
/^\[ RUN      \] Inertialization[^.]*\./ { ran++ }
/^\[       OK \] Inertialization[^.]*\./ { ok++ }
/^\[  PASSED  \] [0-9]+ test/ { passed=$4 }
/\[  FAILED  \]|\[  SKIPPED \]|\[ DISABLED \]|DISABLED TEST|[Ss][Kk][Ii][Pp][Pp][Ee][Dd]/ { bad=1 }
END { if (expected<1 || expected!=ran || ran!=ok || ok!=passed || bad) { print "TARGET_TEST_EXECUTION_INVALID"; exit 1 } }
' "$1"
