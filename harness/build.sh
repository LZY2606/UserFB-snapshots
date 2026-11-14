set -euo pipefail
build_baseline() { cc -std=c99 -O1 -I. harness/baseline.c -lm -o "$1"; }
build_target() { cc -std=c99 -O1 -I. harness/driver.c harness/regression.c -lm -o "$1"; }
