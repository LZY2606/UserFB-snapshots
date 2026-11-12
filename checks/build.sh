set -euo pipefail
build_baseline() { cc -std=c99 -O1 -I. checks/library.c checks/baseline.c -lm -o "$1"; }
build_target() { cc -std=c99 -O1 -I. checks/library.c checks/driver.c checks/regression.c -lm -o "$1"; }
