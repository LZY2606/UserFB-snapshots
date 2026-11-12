set -euo pipefail
build_c_header() { cc -std=c99 -I. -c devcheck/c_header.c -o "$1.c.o"; }
build_baseline() { build_c_header "$1"; c++ -std=c++11 -O1 -pthread -DXATLAS_C_API=1 -I. source/xatlas/xatlas.cpp devcheck/baseline.cpp "$1.c.o" -o "$1"; }
build_target() { build_c_header "$1"; c++ -std=c++11 -O1 -pthread -DXATLAS_C_API=1 -I. source/xatlas/xatlas.cpp devcheck/driver.cpp devcheck/regression.cpp "$1.c.o" -o "$1"; }
